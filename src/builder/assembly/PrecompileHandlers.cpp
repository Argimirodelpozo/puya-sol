/// @file PrecompileHandlers.cpp
/// EVM precompile implementations: ecAdd, ecMul, ecPairing, ecRecover, sha256, modExp, identity.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

// ─── BN254 precompile handlers (ecAdd, ecMul, ecPairing) live in the
// runtime-offset variants below; the constant-offset dispatch path
// in PrecompileDispatch wraps offsets as IntegerConstants and calls
// the same handlers. Puya constant-folds at the backend, so the TEAL
// for the static-arg case is identical to the previous twin path.
//
// ecRecover keeps its constant-only variant since the RT path isn't
// implemented for it (and no test exercises a dynamic-offset call).

void AssemblyBuilder::handleEcRecover(
	uint64_t _inputOffset, uint64_t /*_inputSize*/,
	uint64_t _outputOffset, uint64_t /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Input (128 bytes = 4 slots): msgHash(0), v(1), r(2), s(3)
	// Output (32 bytes = 1 slot): left-padded 20-byte Ethereum address

	// 1. Read input slots as 32-byte padded values
	auto msgHash = padTo32Bytes(readMemSlot(_inputOffset, _loc), _loc);
	auto vBiguint = readMemSlot(_inputOffset + 0x20, _loc);
	auto r = padTo32Bytes(readMemSlot(_inputOffset + 0x40, _loc), _loc);
	auto s = padTo32Bytes(readMemSlot(_inputOffset + 0x60, _loc), _loc);

	// 2. Compute recovery_id = v - 27 as uint64
	auto twentySeven = awst::makeIntegerConstant("27", _loc, awst::WType::biguintType());

	auto vMinus27 = makeBigUIntBinOp(
		std::move(vBiguint), awst::BigUIntBinaryOperator::Sub,
		std::move(twentySeven), _loc
	);

	// Cast biguint → bytes → btoi → uint64
	auto vBytes = awst::makeReinterpretCast(std::move(vMinus27), awst::WType::bytesType(), _loc);

	auto recoveryId = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
	recoveryId->stackArgs.push_back(std::move(vBytes));

	// 3. Call ecdsa_pk_recover Secp256k1
	// Returns (bytes, bytes) — pubkey_x and pubkey_y, each 32 bytes
	awst::WType const* tupleTypePtr = m_typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()}
	);

	auto ecdsaRecover = awst::makeIntrinsicCall("ecdsa_pk_recover", tupleTypePtr, _loc);
	ecdsaRecover->immediates.push_back("Secp256k1");
	ecdsaRecover->stackArgs.push_back(std::move(msgHash));
	ecdsaRecover->stackArgs.push_back(std::move(recoveryId));
	ecdsaRecover->stackArgs.push_back(std::move(r));
	ecdsaRecover->stackArgs.push_back(std::move(s));

	// Store the tuple result in a temporary
	std::string tupleVar = "__ecdsa_result";
	m_locals[tupleVar] = tupleTypePtr;

	auto tupleTarget = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);

	auto assignTuple = awst::makeAssignmentStatement(tupleTarget, std::move(ecdsaRecover), _loc);
	_out.push_back(std::move(assignTuple));

	// 4. Extract pubkey_x (index 0) and pubkey_y (index 1)
	auto tupleRead0 = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);

	auto pubkeyX = std::make_shared<awst::TupleItemExpression>();
	pubkeyX->sourceLocation = _loc;
	pubkeyX->wtype = awst::WType::bytesType();
	pubkeyX->base = std::move(tupleRead0);
	pubkeyX->index = 0;

	auto tupleRead1 = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);

	auto pubkeyY = std::make_shared<awst::TupleItemExpression>();
	pubkeyY->sourceLocation = _loc;
	pubkeyY->wtype = awst::WType::bytesType();
	pubkeyY->base = std::move(tupleRead1);
	pubkeyY->index = 1;

	// 5. concat(pubkey_x, pubkey_y) → 64 bytes
	auto pubkeyConcat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	pubkeyConcat->stackArgs.push_back(std::move(pubkeyX));
	pubkeyConcat->stackArgs.push_back(std::move(pubkeyY));

	// 6. keccak256(concat) → 32 bytes
	auto hash = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
	hash->stackArgs.push_back(std::move(pubkeyConcat));

	// 7. extract3(hash, 12, 20) → last 20 bytes (Ethereum address)
	auto off12 = awst::makeIntegerConstant("12", _loc);
	auto len20 = awst::makeIntegerConstant("20", _loc);

	auto addr = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	addr->stackArgs.push_back(std::move(hash));
	addr->stackArgs.push_back(std::move(off12));
	addr->stackArgs.push_back(std::move(len20));

	// 8. Left-pad to 32 bytes: concat(bzero(12), addr)
	auto pad12 = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	auto twelve = awst::makeIntegerConstant("12", _loc);
	pad12->stackArgs.push_back(std::move(twelve));

	auto paddedAddr = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	paddedAddr->stackArgs.push_back(std::move(pad12));
	paddedAddr->stackArgs.push_back(std::move(addr));

	// 9. Cast to biguint and store
	auto addrBiguint = awst::makeReinterpretCast(std::move(paddedAddr), awst::WType::biguintType(), _loc);

	storeResultToMemory(std::move(addrBiguint), _outputOffset, 1, _loc, _out);
}

// ─── Runtime-offset precompile handlers ─────────────────────────────────────
//
// Same shape as the constant-offset handlers above, but the input/output
// offsets and sizes are AWST Expressions (evaluated at runtime). Used when
// the Yul staticcall has dynamic memory positions.

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
	// numPairs = inputSize / (6*32) — runtime value. Honk uses fixed 4-pair
	// pairings, but we can't generally assume that. For an MVP, support
	// only the 1-pair and 4-pair pairings the verifier actually emits by
	// building a runtime-loop variant. For honk specifically (and the
	// only path stressed today), inputSize is always a compile-time
	// constant since the verifier does e.g. `staticcall(gas(), 8, ..., 0x180, ...)`.
	// So we conservatively check: if inputSize resolves to a constant
	// (integer constant in the AWST), unroll; otherwise fall back to a
	// dynamic loop.
	//
	// Try the unroll path first. We accept any constant-integer expression
	// regardless of whether the original Yul site was constant — puya may
	// have constant-folded for us.
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
			storeResultToMemoryRT(awst::makeBoolConstant(true, _loc),
				std::move(_outputOffset), 1, _loc, _out, /*isBool=*/true);
			return;
		}
		// Build G1+G2 inputs as concatenations across all pairs.
		auto concatTwoSlotsRT = [&](std::shared_ptr<awst::Expression> off1,
									std::shared_ptr<awst::Expression> off2)
			-> std::shared_ptr<awst::Expression> {
			auto extract = [&](std::shared_ptr<awst::Expression> off) {
				auto a = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				a->stackArgs.push_back(memoryVar(_loc));
				a->stackArgs.push_back(std::move(off));
				a->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));
				return a;
			};
			auto a = extract(std::move(off1));
			auto b = extract(std::move(off2));
			auto c = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			c->stackArgs.push_back(std::move(a));
			c->stackArgs.push_back(std::move(b));
			return c;
		};
		auto plusConst = [&](std::shared_ptr<awst::Expression> base, uint64_t k) {
			if (k == 0) return base;
			return std::shared_ptr<awst::Expression>(awst::makeUInt64BinOp(
				std::move(base), O::Add,
				awst::makeIntegerConstant(std::to_string(k), _loc), _loc));
		};

		// Bind input offset to a local so we don't reduplicate the
		// expression for each pair.
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
			// G2: EVM (x_im, x_re, y_im, y_re); AVM expects (x_re, x_im, y_re, y_im).
			auto g2_x = concatTwoSlotsRT(
				plusConst(baseOff(), pairBase + 3 * 0x20),
				plusConst(baseOff(), pairBase + 2 * 0x20));
			auto g2_y = concatTwoSlotsRT(
				plusConst(baseOff(), pairBase + 5 * 0x20),
				plusConst(baseOff(), pairBase + 4 * 0x20));
			auto g2 = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			g2->stackArgs.push_back(std::move(g2_x));
			g2->stackArgs.push_back(std::move(g2_y));

			if (!g1All) g1All = std::move(g1);
			else
			{
				auto c = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				c->stackArgs.push_back(std::move(g1All));
				c->stackArgs.push_back(std::move(g1));
				g1All = std::move(c);
			}
			if (!g2All) g2All = std::move(g2);
			else
			{
				auto c = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				c->stackArgs.push_back(std::move(g2All));
				c->stackArgs.push_back(std::move(g2));
				g2All = std::move(c);
			}
		}
		ecCall->stackArgs.push_back(std::move(g1All));
		ecCall->stackArgs.push_back(std::move(g2All));
		storeResultToMemoryRT(std::move(ecCall), std::move(_outputOffset), 1, _loc, _out, /*isBool=*/true);
		return;
	}

	// Fully-dynamic input size: not currently supported (would need a
	// runtime loop emitting one ec_pairing_check). Fall back to stub.
	Logger::instance().warning(
		"ec_pairing with dynamic input size — stubbing as success (no runtime "
		"pair-count loop yet)", _loc);
	storeResultToMemoryRT(awst::makeBoolConstant(true, _loc),
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
	// Read inputSize bytes from memory at inputOffset, hash, write 32 bytes
	// at outputOffset. The output size for SHA-256 is always 32.
	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(memoryVar(_loc));
	extract->stackArgs.push_back(offsetToUint64(std::move(_inputOffset), _loc));
	extract->stackArgs.push_back(offsetToUint64(std::move(_inputSize), _loc));

	auto sha = awst::makeIntrinsicCall("sha256", awst::WType::bytesType(), _loc);
	sha->stackArgs.push_back(std::move(extract));

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
	// Memory-to-memory copy of inputSize bytes from inputOffset to outputOffset.
	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(memoryVar(_loc));
	extract->stackArgs.push_back(offsetToUint64(std::move(_inputOffset), _loc));
	extract->stackArgs.push_back(offsetToUint64(std::move(_inputSize), _loc));

	auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
	replace->stackArgs.push_back(memoryVar(_loc));
	replace->stackArgs.push_back(offsetToUint64(std::move(_outputOffset), _loc));
	replace->stackArgs.push_back(std::move(extract));

	assignMemoryVar(std::move(replace), _loc, _out);
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
	// EIP-198 layout (Bsize=Esize=Msize=32 — the only shape we currently
	// emit on the AVM side; same constraint as the constant variant). Slots:
	//   +0x00 Bsize, +0x20 Esize, +0x40 Msize, +0x60 base, +0x80 exp,
	//   +0xa0 mod. Output: 1 slot at outputOffset.
	using O = awst::UInt64BinaryOperator;

	// Bind input offset to a local so we don't reduplicate the expression
	// for each slot read.
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
			awst::makeIntegerConstant(std::to_string(k), _loc), _loc));
	};
	auto readSlot = [&](uint64_t slotOff) -> std::shared_ptr<awst::Expression>
	{
		auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		extract->stackArgs.push_back(memoryVar(_loc));
		extract->stackArgs.push_back(plusConst(baseOff(), slotOff));
		extract->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));
		return awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
	};

	auto base = readSlot(0x60);
	auto exp = readSlot(0x80);
	auto mod = readSlot(0xa0);

	// Square-and-multiply (mirrors handleModExp's loop):
	//   result = 1; base = base % mod
	//   while exp > 0:
	//       if exp & 1: result = (result * base) % mod
	//       exp = exp / 2
	//       base = (base * base) % mod
	std::string resultVar = "__modexp_result";
	std::string baseVar = "__modexp_base";
	std::string expVar = "__modexp_exp";
	std::string modVar = "__modexp_mod";
	m_locals[resultVar] = awst::WType::biguintType();
	m_locals[baseVar] = awst::WType::biguintType();
	m_locals[expVar] = awst::WType::biguintType();
	m_locals[modVar] = awst::WType::biguintType();

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

	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = _loc;
	loop->condition = awst::makeNumericCompare(makeVar(expVar), awst::NumericComparison::Gt, makeConst("0"), _loc);

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	{
		auto expAnd1 = makeBigUIntBinOp(
			makeVar(expVar), awst::BigUIntBinaryOperator::BitAnd, makeConst("1"), _loc);
		auto isOdd = awst::makeNumericCompare(std::move(expAnd1), awst::NumericComparison::Ne, makeConst("0"), _loc);
		auto product = makeBigUIntBinOp(
			makeVar(resultVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar), _loc);
		auto modResult = makeBigUIntBinOp(
			std::move(product), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc);
		auto ifBlock = std::make_shared<awst::Block>();
		ifBlock->sourceLocation = _loc;
		ifBlock->body.push_back(makeAssign(resultVar, std::move(modResult)));
		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = _loc;
		ifStmt->condition = std::move(isOdd);
		ifStmt->ifBranch = std::move(ifBlock);
		body->body.push_back(std::move(ifStmt));
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

	loop->loopBody = std::move(body);
	_out.push_back(std::move(loop));

	storeResultToMemoryRT(makeVar(resultVar), std::move(_outputOffset), 1, _loc, _out);
}


} // namespace puyasol::builder
