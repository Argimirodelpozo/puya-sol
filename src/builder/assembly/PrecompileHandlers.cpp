/// @file PrecompileHandlers.cpp
/// EVM precompile implementations: ecAdd, ecMul, ecPairing, ecRecover, sha256, modExp, identity.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

void AssemblyBuilder::handleEcAdd(
	uint64_t _inputOffset, uint64_t _outputOffset,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// ecAdd: 4 input slots (x1,y1,x2,y2), 2 output slots (rx,ry)
	auto ecCall = awst::makeIntrinsicCall("ec_add", awst::WType::bytesType(), _loc);
	ecCall->immediates.push_back("BN254g1");
	ecCall->stackArgs.push_back(concatSlots(_inputOffset, 0, 2, _loc)); // point A
	ecCall->stackArgs.push_back(concatSlots(_inputOffset, 2, 2, _loc)); // point B
	storeResultToMemory(std::move(ecCall), _outputOffset, 2, _loc, _out);
}

void AssemblyBuilder::handleEcMul(
	uint64_t _inputOffset, uint64_t _outputOffset,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// ecMul: 3 input slots (x,y,s), 2 output slots (rx,ry)
	auto ecCall = awst::makeIntrinsicCall("ec_scalar_mul", awst::WType::bytesType(), _loc);
	ecCall->immediates.push_back("BN254g1");
	ecCall->stackArgs.push_back(concatSlots(_inputOffset, 0, 2, _loc)); // point A
	ecCall->stackArgs.push_back(concatSlots(_inputOffset, 2, 1, _loc)); // scalar
	storeResultToMemory(std::move(ecCall), _outputOffset, 2, _loc, _out);
}

void AssemblyBuilder::handleEcPairing(
	uint64_t _inputOffset, uint64_t _inputSize,
	uint64_t _outputOffset,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// ecPairing: variable inputs (6 words per pair), 1 output (bool)
	int inputSlots = static_cast<int>(_inputSize / 0x20);
	int numPairs = inputSlots / 6;

	auto ecCall = awst::makeIntrinsicCall("ec_pairing_check", awst::WType::boolType(), _loc);
	ecCall->immediates.push_back("BN254g1");

	if (numPairs > 0)
	{
		// Helper to concatenate two padded slots at absolute offsets
		auto concatTwoAbsSlots = [&](uint64_t offA, uint64_t offB) -> std::shared_ptr<awst::Expression>
		{
			auto a = padTo32Bytes(readMemSlot(offA, _loc), _loc);
			auto b = padTo32Bytes(readMemSlot(offB, _loc), _loc);
			auto c = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			c->stackArgs.push_back(std::move(a));
			c->stackArgs.push_back(std::move(b));
			return c;
		};

		std::shared_ptr<awst::Expression> g1Points;
		std::shared_ptr<awst::Expression> g2Points;
		for (int p = 0; p < numPairs; ++p)
		{
			uint64_t pairBase = _inputOffset + static_cast<uint64_t>(p * 6) * 0x20;
			// G1 point: 2 words (x, y) — same ordering in EVM and AVM
			auto g1 = concatSlots(_inputOffset, p * 6, 2, _loc);
			// G2 point: EVM stores as (x_im, x_re, y_im, y_re)
			// AVM expects (x_re, x_im, y_re, y_im) — swap within each coordinate pair
			auto g2_x = concatTwoAbsSlots(
				pairBase + 3 * 0x20, pairBase + 2 * 0x20 // x_re, x_im
			);
			auto g2_y = concatTwoAbsSlots(
				pairBase + 5 * 0x20, pairBase + 4 * 0x20 // y_re, y_im
			);
			auto g2 = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			g2->stackArgs.push_back(std::move(g2_x));
			g2->stackArgs.push_back(std::move(g2_y));

			if (!g1Points)
				g1Points = std::move(g1);
			else
			{
				auto c = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				c->stackArgs.push_back(std::move(g1Points));
				c->stackArgs.push_back(std::move(g1));
				g1Points = std::move(c);
			}
			if (!g2Points)
				g2Points = std::move(g2);
			else
			{
				auto c = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				c->stackArgs.push_back(std::move(g2Points));
				c->stackArgs.push_back(std::move(g2));
				g2Points = std::move(c);
			}
		}
		if (g1Points) ecCall->stackArgs.push_back(std::move(g1Points));
		if (g2Points) ecCall->stackArgs.push_back(std::move(g2Points));
	}

	storeResultToMemory(std::move(ecCall), _outputOffset, 1, _loc, _out, /*_isBoolResult=*/true);
}

// ─── New precompile handlers ────────────────────────────────────────────────

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

void AssemblyBuilder::handleSha256Precompile(
	uint64_t _inputOffset, uint64_t _inputSize,
	uint64_t _outputOffset, uint64_t /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Concatenate input memory slots
	int numFullSlots = static_cast<int>(_inputSize / 0x20);
	uint64_t remainder = _inputSize % 0x20;

	std::shared_ptr<awst::Expression> inputData;
	if (numFullSlots > 0)
		inputData = concatSlots(_inputOffset, 0, numFullSlots, _loc);

	// Handle partial last slot
	if (remainder > 0)
	{
		uint64_t partialOff = _inputOffset + static_cast<uint64_t>(numFullSlots) * 0x20;
		auto partialSlot = padTo32Bytes(readMemSlot(partialOff, _loc), _loc);

		// Truncate to just the remainder bytes: extract3(padded, 0, remainder)
		auto offZero = awst::makeIntegerConstant("0", _loc);
		auto remLen = awst::makeIntegerConstant(std::to_string(remainder), _loc);

		auto truncated = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		truncated->stackArgs.push_back(std::move(partialSlot));
		truncated->stackArgs.push_back(std::move(offZero));
		truncated->stackArgs.push_back(std::move(remLen));

		if (!inputData)
			inputData = std::move(truncated);
		else
		{
			auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			concat->stackArgs.push_back(std::move(inputData));
			concat->stackArgs.push_back(std::move(truncated));
			inputData = std::move(concat);
		}
	}

	if (!inputData)
	{
		// Empty input: sha256 of empty bytes
		inputData = awst::makeBytesConstant({}, _loc, awst::BytesEncoding::Unknown);
	}

	// If inputSize is a multiple of 32, also truncate to exact size
	// (concatSlots may include full 32-byte slots when input is exact)
	if (remainder == 0 && numFullSlots > 0)
	{
		// No truncation needed — concatSlots gives exactly numFullSlots*32 = inputSize
	}

	// Apply sha256
	auto sha256Call = awst::makeIntrinsicCall("sha256", awst::WType::bytesType(), _loc);
	sha256Call->stackArgs.push_back(std::move(inputData));

	// Convert to biguint and store at output
	auto castResult = awst::makeReinterpretCast(std::move(sha256Call), awst::WType::biguintType(), _loc);

	storeResultToMemory(std::move(castResult), _outputOffset, 1, _loc, _out);
}

void AssemblyBuilder::handleModExp(
	uint64_t _inputOffset, uint64_t /*_inputSize*/,
	uint64_t _outputOffset, uint64_t /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// ModExp input format (EIP-198):
	//   slot 0 (offset+0x00): Bsize (length of base in bytes)
	//   slot 1 (offset+0x20): Esize (length of exponent in bytes)
	//   slot 2 (offset+0x40): Msize (length of modulus in bytes)
	//   Then Bsize bytes of base, Esize bytes of exp, Msize bytes of mod
	//   packed contiguously starting at offset+0x60.
	//
	// Common Solidity usage: Bsize=Esize=Msize=32 (one slot each), so:
	//   slot 3 (offset+0x60): base
	//   slot 4 (offset+0x80): exp
	//   slot 5 (offset+0xa0): mod
	//   Output: one 32-byte slot with base^exp % mod
	//
	// We currently only support the fixed 32/32/32 case.
	// TODO: For variable-length inputs, parse Bsize/Esize/Msize dynamically.

	// Read Bsize, Esize, Msize to verify they're the common 32/32/32 pattern
	auto bsizeOpt = resolveConstantOffset(readMemSlot(_inputOffset, _loc));
	auto esizeOpt = resolveConstantOffset(readMemSlot(_inputOffset + 0x20, _loc));
	auto msizeOpt = resolveConstantOffset(readMemSlot(_inputOffset + 0x40, _loc));

	if (bsizeOpt && esizeOpt && msizeOpt
		&& *bsizeOpt != 32 && *esizeOpt != 32 && *msizeOpt != 32)
	{
		Logger::instance().warning(
			"modexp with non-32-byte operands (Bsize=" + std::to_string(*bsizeOpt) +
			", Esize=" + std::to_string(*esizeOpt) +
			", Msize=" + std::to_string(*msizeOpt) +
			"); proceeding with slot-based reads which may be incorrect", _loc
		);
	}

	// Read base, exp, mod from slots 3, 4, 5
	auto base = readMemSlot(_inputOffset + 0x60, _loc);
	auto exp = readMemSlot(_inputOffset + 0x80, _loc);
	auto mod = readMemSlot(_inputOffset + 0xa0, _loc);

	// Implement modular exponentiation via square-and-multiply:
	//
	//   __modexp_result = 1
	//   __modexp_base = base % mod
	//   __modexp_exp = exp
	//   while __modexp_exp > 0:
	//       if __modexp_exp & 1 != 0:       // exp is odd
	//           __modexp_result = (__modexp_result * __modexp_base) % mod
	//       __modexp_exp = __modexp_exp / 2
	//       __modexp_base = (__modexp_base * __modexp_base) % mod
	//   // result is in __modexp_result

	std::string resultVar = "__modexp_result";
	std::string baseVar = "__modexp_base";
	std::string expVar = "__modexp_exp";
	std::string modVar = "__modexp_mod";

	m_locals[resultVar] = awst::WType::biguintType();
	m_locals[baseVar] = awst::WType::biguintType();
	m_locals[expVar] = awst::WType::biguintType();
	m_locals[modVar] = awst::WType::biguintType();

	// Helper: make a VarExpression
	auto makeVar = [&](std::string const& name) -> std::shared_ptr<awst::VarExpression>
	{
		auto v = awst::makeVarExpression(name, awst::WType::biguintType(), _loc);
		return v;
	};

	// Helper: make a biguint constant
	auto makeConst = [&](std::string const& value) -> std::shared_ptr<awst::IntegerConstant>
	{
		auto c = awst::makeIntegerConstant(value, _loc, awst::WType::biguintType());
		return c;
	};

	// Helper: make an assignment statement
	auto makeAssign = [&](
		std::string const& target,
		std::shared_ptr<awst::Expression> value
	) -> std::shared_ptr<awst::AssignmentStatement>
	{
		auto assign = awst::makeAssignmentStatement(makeVar(target), std::move(value), _loc);
		return assign;
	};

	// __modexp_mod = mod
	_out.push_back(makeAssign(modVar, std::move(mod)));

	// __modexp_result = 1
	_out.push_back(makeAssign(resultVar, makeConst("1")));

	// __modexp_base = base % mod
	_out.push_back(makeAssign(baseVar,
		makeBigUIntBinOp(std::move(base), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc)
	));

	// __modexp_exp = exp
	_out.push_back(makeAssign(expVar, std::move(exp)));

	// Build loop: while __modexp_exp > 0
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = _loc;

	// Condition: __modexp_exp > 0
	auto cond = awst::makeNumericCompare(makeVar(expVar), awst::NumericComparison::Gt, makeConst("0"), _loc);
	loop->condition = std::move(cond);

	// Loop body
	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// 1. if __modexp_exp & 1 != 0:
	//        __modexp_result = (__modexp_result * __modexp_base) % __modexp_mod
	{
		// __modexp_exp & 1: use BigUIntBinaryOperation with BitAnd
		auto expAnd1 = makeBigUIntBinOp(
			makeVar(expVar), awst::BigUIntBinaryOperator::BitAnd, makeConst("1"), _loc
		);

		// expAnd1 != 0
		auto isOdd = awst::makeNumericCompare(std::move(expAnd1), awst::NumericComparison::Ne, makeConst("0"), _loc);

		// result = (result * base) % mod
		auto product = makeBigUIntBinOp(
			makeVar(resultVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar), _loc
		);
		auto modResult = makeBigUIntBinOp(
			std::move(product), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc
		);

		auto ifBlock = std::make_shared<awst::Block>();
		ifBlock->sourceLocation = _loc;
		ifBlock->body.push_back(makeAssign(resultVar, std::move(modResult)));

		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = _loc;
		ifStmt->condition = std::move(isOdd);
		ifStmt->ifBranch = std::move(ifBlock);

		body->body.push_back(std::move(ifStmt));
	}

	// 2. __modexp_exp = __modexp_exp / 2
	body->body.push_back(makeAssign(expVar,
		makeBigUIntBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::FloorDiv, makeConst("2"), _loc)
	));

	// 3. __modexp_base = (__modexp_base * __modexp_base) % __modexp_mod
	{
		auto squared = makeBigUIntBinOp(
			makeVar(baseVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar), _loc
		);
		auto modSquared = makeBigUIntBinOp(
			std::move(squared), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc
		);
		body->body.push_back(makeAssign(baseVar, std::move(modSquared)));
	}

	loop->loopBody = std::move(body);
	_out.push_back(std::move(loop));

	// Store __modexp_result at the output offset
	storeResultToMemory(makeVar(resultVar), _outputOffset, 1, _loc, _out);
}

void AssemblyBuilder::handleIdentityPrecompile(
	uint64_t _inputOffset, uint64_t _inputSize,
	uint64_t _outputOffset, uint64_t /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Identity precompile: copy input region to output region in the memory blob.
	// extract3(__evm_memory, inputOffset, inputSize) → replace3(__evm_memory, outputOffset, data)
	auto inOffConst = awst::makeIntegerConstant(std::to_string(_inputOffset), _loc);

	auto inSizeConst = awst::makeIntegerConstant(std::to_string(_inputSize), _loc);

	auto extractData = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extractData->stackArgs.push_back(memoryVar(_loc));
	extractData->stackArgs.push_back(std::move(inOffConst));
	extractData->stackArgs.push_back(std::move(inSizeConst));

	auto outOffConst = awst::makeIntegerConstant(std::to_string(_outputOffset), _loc);

	auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
	replace->stackArgs.push_back(memoryVar(_loc));
	replace->stackArgs.push_back(std::move(outOffConst));
	replace->stackArgs.push_back(std::move(extractData));

	assignMemoryVar(std::move(replace), _loc, _out);
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
