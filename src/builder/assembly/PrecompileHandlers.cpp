/// @file PrecompileHandlers.cpp
/// EVM precompile implementations: ecAdd, ecMul, ecPairing, ecRecover, sha256, modExp, identity.

#include "builder/assembly/AssemblyBuilder.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

// ecRecover: constant-only (no RT path implemented; no test exercises dynamic offsets).
// ecAdd/ecMul/ecPairing/SHA-256/Identity: RT handlers below; constant-offset dispatch
// wraps as IntegerConstants and calls the same handlers (puya folds at backend).

void AssemblyBuilder::handleEcRecover(
	uint64_t _inputOffset, uint64_t /*_inputSize*/,
	uint64_t _outputOffset, uint64_t /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Input (128 bytes): msgHash(+0), v(+0x20), r(+0x40), s(+0x60).
	// Output: left-padded 20-byte Ethereum address (1 slot).
	auto msgHash = padTo32Bytes(readMemSlot(_inputOffset, _loc), _loc);
	auto vBiguint = readMemSlot(_inputOffset + 0x20, _loc);
	auto r = padTo32Bytes(readMemSlot(_inputOffset + 0x40, _loc), _loc);
	auto s = padTo32Bytes(readMemSlot(_inputOffset + 0x60, _loc), _loc);

	// Bind v once and gate the AVM intrinsic to exactly 27/28. Invalid v makes
	// EVM ecrecover produce no output, so the destination memory stays intact.
	std::string vName = "__ecdsa_v_"
		+ std::to_string(awst::NameGen::next("AssemblyBuilder.ecrecoverV"));
	m_locals[vName] = awst::WType::biguintType();
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(vName, awst::WType::biguintType(), _loc),
		std::move(vBiguint), _loc));
	auto vRead = [&] {
		return awst::makeVarExpression(vName, awst::WType::biguintType(), _loc);
	};
	auto validV = awst::makeBoolBinOp(
		awst::makeNumericCompare(vRead(), awst::NumericComparison::Eq,
			awst::makeBiguintConstant("27", _loc), _loc),
		awst::BinaryBooleanOperator::Or,
		awst::makeNumericCompare(vRead(), awst::NumericComparison::Eq,
			awst::makeBiguintConstant("28", _loc), _loc), _loc);
	auto vMinus27 = makeBigUIntBinOp(
		vRead(), awst::BigUIntBinaryOperator::Sub,
		awst::makeBiguintConstant("27", _loc), _loc);
	auto recoveryId = safeBtoi(std::move(vMinus27), _loc);
	auto validBlock = awst::makeBlock(_loc);

	awst::WType const* tupleTypePtr = m_typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()}
	);
	auto ecdsaRecover = awst::makeIntrinsicCall("ecdsa_pk_recover", tupleTypePtr, _loc);
	ecdsaRecover->immediates.push_back("Secp256k1");
	ecdsaRecover->stackArgs.push_back(std::move(msgHash));
	ecdsaRecover->stackArgs.push_back(std::move(recoveryId));
	ecdsaRecover->stackArgs.push_back(std::move(r));
	ecdsaRecover->stackArgs.push_back(std::move(s));

	std::string tupleVar = "__ecdsa_result";
	m_locals[tupleVar] = tupleTypePtr;
	validBlock->body.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(tupleVar, tupleTypePtr, _loc), std::move(ecdsaRecover), _loc));

	// keccak256(concat(pubkey_x, pubkey_y)) → last 20 bytes → left-pad to 32
	auto pubkeyX = awst::makeTupleItem(
		awst::makeVarExpression(tupleVar, tupleTypePtr, _loc), 0, awst::WType::bytesType(), _loc);
	auto pubkeyY = awst::makeTupleItem(
		awst::makeVarExpression(tupleVar, tupleTypePtr, _loc), 1, awst::WType::bytesType(), _loc);
	auto hash = awst::makeKeccak256(
		awst::makeConcat(std::move(pubkeyX), std::move(pubkeyY), _loc), _loc);
	auto addr = awst::makeExtract(std::move(hash), 12, 20, _loc);
	storeResultToMemory(
		awst::makeAsBiguint(awst::makeLeftPad(std::move(addr), 12, _loc), _loc),
		_outputOffset, 1, _loc, validBlock->body);
	_out.push_back(awst::makeIfElse(
		std::move(validV), std::move(validBlock), nullptr, _loc));
}

// ─── Runtime-offset precompile handlers ─────────────────────────────────────
// Same as the constant-offset handlers but offsets/sizes are AWST Expressions.

void AssemblyBuilder::handleEcAddRT(
	std::shared_ptr<awst::Expression> _inputOffset,
	std::shared_ptr<awst::Expression> _outputOffset,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto ecCall = awst::makeIntrinsicCall("ec_add", awst::WType::bytesType(), _loc);
	ecCall->immediates.push_back("BN254g1");
	ecCall->stackArgs.push_back(concatSlotsRT(_inputOffset, 0, 2, _loc));  // point A
	ecCall->stackArgs.push_back(concatSlotsRT(_inputOffset, 2, 2, _loc));  // point B
	storeResultToMemoryRT(std::move(ecCall), std::move(_outputOffset), 2, _loc, _out);
}

void AssemblyBuilder::handleEcMulRT(
	std::shared_ptr<awst::Expression> _inputOffset,
	std::shared_ptr<awst::Expression> _outputOffset,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto ecCall = awst::makeIntrinsicCall("ec_scalar_mul", awst::WType::bytesType(), _loc);
	ecCall->immediates.push_back("BN254g1");
	ecCall->stackArgs.push_back(concatSlotsRT(_inputOffset, 0, 2, _loc));  // point
	ecCall->stackArgs.push_back(concatSlotsRT(_inputOffset, 2, 1, _loc));  // scalar
	storeResultToMemoryRT(std::move(ecCall), std::move(_outputOffset), 2, _loc, _out);
}

void AssemblyBuilder::handleEcPairingRT(
	std::shared_ptr<awst::Expression> _inputOffset,
	std::shared_ptr<awst::Expression> _inputSize,
	std::shared_ptr<awst::Expression> _outputOffset,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	using O = awst::UInt64BinaryOperator;
	// numPairs = inputSize / (6*32). Unroll when inputSize is a compile-time constant
	// (honk emits e.g. `staticcall(gas(), 8, ..., 0x180, ...)` so this always fires).
	// Accept any IntegerConstant regardless of origin — puya may have folded it.
	auto* sizeConst = dynamic_cast<awst::IntegerConstant const*>(_inputSize.get());
	if (sizeConst)
	{
		uint64_t inSize = std::stoull(sizeConst->value);
		int inputSlots = static_cast<int>(inSize / 0x20);
		int numPairs = inputSlots / 6;
		auto ecCall = awst::makeIntrinsicCall("ec_pairing_check", awst::WType::boolType(), _loc);
		ecCall->immediates.push_back("BN254g1");
		if (numPairs <= 0)
		{
			// Empty pairing: AVM ec_pairing_check needs at least one pair;
			// emit `true` directly.
			storeResultToMemoryRT(awst::makeTrue(_loc),
				std::move(_outputOffset), 1, _loc, _out, /*isBool=*/true);
			return;
		}
		// Build G1+G2 inputs as concatenations across all pairs.
		// readMemWordDyn is slot-aware: local for slot 0, loads() for slot 1+.
		auto concatTwoSlotsRT = [&](std::shared_ptr<awst::Expression> off1,
									std::shared_ptr<awst::Expression> off2)
			-> std::shared_ptr<awst::Expression> {
			return awst::makeConcat(
				readMemWordDyn(std::move(off1), _loc),
				readMemWordDyn(std::move(off2), _loc), _loc);
		};
		auto plusConst = [&](std::shared_ptr<awst::Expression> base, uint64_t k) {
			if (k == 0) return base;
			return std::shared_ptr<awst::Expression>(awst::makeUInt64BinOp(
				std::move(base), O::Add,
				awst::makeIntegerConstant(k, _loc), _loc));
		};

		// Bind input offset to a local to avoid re-evaluating it per pair.
		std::string inOffVar = "__pairing_in_off";
		m_locals[inOffVar] = awst::WType::uint64Type();
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(inOffVar, awst::WType::uint64Type(), _loc),
			offsetToUint64(std::move(_inputOffset), _loc), _loc));
		auto baseOff = [&]() {
			return awst::makeVarExpression(inOffVar, awst::WType::uint64Type(), _loc);
		};

		std::shared_ptr<awst::Expression> g1All;
		std::shared_ptr<awst::Expression> g2All;
		for (int p = 0; p < numPairs; ++p)
		{
			uint64_t pairBase = static_cast<uint64_t>(p) * 6 * 0x20;
			// G1: 2 slots starting at pairBase.
			auto g1 = concatSlotsRT(plusConst(baseOff(), pairBase), 0, 2, _loc);
			// G2: swap EVM order (x_im, x_re, y_im, y_re) → AVM (x_re, x_im, y_re, y_im).
			auto g2_x = concatTwoSlotsRT(
				plusConst(baseOff(), pairBase + 3 * 0x20),
				plusConst(baseOff(), pairBase + 2 * 0x20));
			auto g2_y = concatTwoSlotsRT(
				plusConst(baseOff(), pairBase + 5 * 0x20),
				plusConst(baseOff(), pairBase + 4 * 0x20));
			auto g2 = awst::makeConcat(std::move(g2_x), std::move(g2_y), _loc);

			if (!g1All) g1All = std::move(g1);
			else g1All = awst::makeConcat(std::move(g1All), std::move(g1), _loc);
			if (!g2All) g2All = std::move(g2);
			else g2All = awst::makeConcat(std::move(g2All), std::move(g2), _loc);
		}
		ecCall->stackArgs.push_back(std::move(g1All));
		ecCall->stackArgs.push_back(std::move(g2All));
		storeResultToMemoryRT(std::move(ecCall), std::move(_outputOffset), 1, _loc, _out, /*isBool=*/true);
		return;
	}

	// Fully-dynamic input size not supported (needs runtime loop per pair).
	// HARD ERROR: stubbing success would make a zk verifier accept any proof.
	Logger::instance().error(
		"ec_pairing (bn256 pairing precompile 0x08) with a dynamic input size "
		"is not supported on AVM — there is no runtime pair-count loop yet, so "
		"the check cannot be computed. Stubbing it as success would make a "
		"pairing/zk verifier accept any proof. Use a constant (compile-time) "
		"number of pairs.", _loc);
	storeResultToMemoryRT(awst::makeTrue(_loc),
		std::move(_outputOffset), 1, _loc, _out, /*isBool=*/true);
}

void AssemblyBuilder::handleSha256PrecompileRT(
	std::shared_ptr<awst::Expression> _inputOffset,
	std::shared_ptr<awst::Expression> _inputSize,
	std::shared_ptr<awst::Expression> _outputOffset,
	std::shared_ptr<awst::Expression> /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// SHA-256 output is always 32 bytes. Input gathered slot-aware at runtime
	// offsets — the old single-slot memoryVar extract read the wrong (or a
	// too-short) buffer once the input lived past SLOT_SIZE.
	auto input = readMemRangeDyn(
		std::move(_inputOffset), std::move(_inputSize), _loc, _out);
	auto sha = awst::makeIntrinsicCall("sha256", awst::WType::bytesType(), _loc);
	sha->stackArgs.push_back(std::move(input));

	storeResultToMemoryRT(std::move(sha), std::move(_outputOffset), 1, _loc, _out);
}

void AssemblyBuilder::handleIdentityPrecompileRT(
	std::shared_ptr<awst::Expression> _inputOffset,
	std::shared_ptr<awst::Expression> _inputSize,
	std::shared_ptr<awst::Expression> _outputOffset,
	std::shared_ptr<awst::Expression> /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Copy inputSize bytes from inputOffset to outputOffset, slot-aware at
	// runtime offsets. The gather snapshots the source first, so overlapping
	// ranges behave like memmove. (The old single-slot extract3/replace3 on
	// memoryVar was silently wrong past SLOT_SIZE — TypedMemView.unsafeCopyTo
	// routes here.)
	auto data = readMemRangeDyn(
		std::move(_inputOffset), std::move(_inputSize), _loc, _out);
	writeMemRangeDyn(std::move(_outputOffset), std::move(data), _loc, _out);
}

void AssemblyBuilder::handleModExpRT(
	std::shared_ptr<awst::Expression> _inputOffset,
	std::shared_ptr<awst::Expression> /*_inputSize*/,
	std::shared_ptr<awst::Expression> _outputOffset,
	std::shared_ptr<awst::Expression> /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// EIP-198 layout (Bsize=Esize=Msize=32 only):
	// +0x00 Bsize, +0x20 Esize, +0x40 Msize, +0x60 base, +0x80 exp, +0xa0 mod.
	using O = awst::UInt64BinaryOperator;

	std::string inOffVar = "__modexp_in_off";
	m_locals[inOffVar] = awst::WType::uint64Type();
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(inOffVar, awst::WType::uint64Type(), _loc),
		offsetToUint64(std::move(_inputOffset), _loc), _loc));

	auto baseOff = [&]() {
		return awst::makeVarExpression(inOffVar, awst::WType::uint64Type(), _loc);
	};
	auto plusConst = [&](std::shared_ptr<awst::Expression> b, uint64_t k)
		-> std::shared_ptr<awst::Expression>
	{
		if (k == 0) return b;
		return std::shared_ptr<awst::Expression>(awst::makeUInt64BinOp(
			std::move(b), O::Add,
			awst::makeIntegerConstant(k, _loc), _loc));
	};
	// readMemWordDyn: slot-0 uses local, slot 1+ uses loads() — needed because modexp
	// input lands at runtime FMP (honk verify ~18KB live; loads-only misses slot 0).
	auto readSlot = [&](uint64_t slotOff) -> std::shared_ptr<awst::Expression>
	{
		return awst::makeAsBiguint(readMemWordDyn(plusConst(baseOff(), slotOff), _loc), _loc);
	};

	// This implementation only supports the 32/32/32 operand layout. The EVM
	// precompile handles arbitrary Bsize/Esize/Msize (RSA-2048 uses a 256-byte
	// modulus); computing on fixed 32-byte windows for any other size would
	// silently produce a wrong result. Assert the three header words (Bsize at
	// +0x00, Esize at +0x20, Msize at +0x40) are each 32, so an unsupported
	// width fails loud instead.
	auto assertHeader32 = [&](uint64_t slotOff, char const* which) {
		auto sz = awst::makeAsBiguint(readMemWordDyn(plusConst(baseOff(), slotOff), _loc), _loc);
		auto ok = awst::makeNumericCompare(std::move(sz), awst::NumericComparison::Eq,
			awst::makeIntegerConstant("32", _loc, awst::WType::biguintType()), _loc);
		_out.push_back(awst::makeExpressionStatement(
			awst::makeAssert(std::move(ok), _loc,
				std::string("modexp precompile only supports 32-byte operands (") + which + ")"), _loc));
	};
	assertHeader32(0x00, "Bsize");
	assertHeader32(0x20, "Esize");
	assertHeader32(0x40, "Msize");

	auto base = readSlot(0x60);
	auto exp = readSlot(0x80);
	auto mod = readSlot(0xa0);

	// Square-and-multiply: result=1; base%=mod; while exp>0: { if exp&1: result=result*base%mod; exp>>=1; base=base*base%mod; }
	std::string resultVar = "__modexp_result";
	std::string baseVar = "__modexp_base";
	std::string expVar = "__modexp_exp";
	std::string modVar = "__modexp_mod";
	for (auto const& v : {resultVar, baseVar, expVar, modVar})
		m_locals[v] = awst::WType::biguintType();

	auto makeVar = [&](std::string const& n) {
		return awst::makeVarExpression(n, awst::WType::biguintType(), _loc);
	};
	auto makeConst = [&](std::string const& v) {
		return awst::makeIntegerConstant(v, _loc, awst::WType::biguintType());
	};
	auto makeAssign = [&](std::string const& t, std::shared_ptr<awst::Expression> v) {
		return awst::makeAssignmentStatement(makeVar(t), std::move(v), _loc);
	};

	_out.push_back(makeAssign(modVar, std::move(mod)));
	_out.push_back(makeAssign(resultVar, makeConst("1")));
	_out.push_back(makeAssign(baseVar,
		makeBigUIntBinOp(std::move(base), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc)
	));
	_out.push_back(makeAssign(expVar, std::move(exp)));

	auto loopCond = awst::makeNumericCompare(makeVar(expVar), awst::NumericComparison::Gt, makeConst("0"), _loc);
	auto body = awst::makeBlock(_loc);

	{
		auto expAnd1 = makeBigUIntBinOp(
			makeVar(expVar), awst::BigUIntBinaryOperator::BitAnd, makeConst("1"), _loc);
		auto isOdd = awst::makeNumericCompare(std::move(expAnd1), awst::NumericComparison::Ne, makeConst("0"), _loc);
		auto product = makeBigUIntBinOp(
			makeVar(resultVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar), _loc);
		auto modResult = makeBigUIntBinOp(
			std::move(product), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc);
		auto ifBlock = awst::makeBlock(_loc);
		ifBlock->body.push_back(makeAssign(resultVar, std::move(modResult)));
		body->body.push_back(awst::makeIfElse(
			std::move(isOdd), std::move(ifBlock), nullptr, _loc));
	}

	body->body.push_back(makeAssign(expVar,
		makeBigUIntBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::FloorDiv, makeConst("2"), _loc)
	));

	{
		auto squared = makeBigUIntBinOp(
			makeVar(baseVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar), _loc);
		auto modSquared = makeBigUIntBinOp(
			std::move(squared), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc);
		body->body.push_back(makeAssign(baseVar, std::move(modSquared)));
	}

	_out.push_back(awst::makeWhileLoop(std::move(loopCond), std::move(body), _loc));

	storeResultToMemoryRT(makeVar(resultVar), std::move(_outputOffset), 1, _loc, _out);
}


} // namespace puyasol::builder
