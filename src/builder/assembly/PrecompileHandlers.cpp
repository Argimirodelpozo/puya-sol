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
	auto ecCall = std::make_shared<awst::IntrinsicCall>();
	ecCall->sourceLocation = _loc;
	ecCall->wtype = awst::WType::bytesType();
	ecCall->opCode = "ec_add";
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
	auto ecCall = std::make_shared<awst::IntrinsicCall>();
	ecCall->sourceLocation = _loc;
	ecCall->wtype = awst::WType::bytesType();
	ecCall->opCode = "ec_scalar_mul";
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

	auto ecCall = std::make_shared<awst::IntrinsicCall>();
	ecCall->sourceLocation = _loc;
	ecCall->wtype = awst::WType::boolType();
	ecCall->opCode = "ec_pairing_check";
	ecCall->immediates.push_back("BN254g1");

	if (numPairs > 0)
	{
		// Helper to concatenate two padded slots at absolute offsets
		auto concatTwoAbsSlots = [&](uint64_t offA, uint64_t offB) -> std::shared_ptr<awst::Expression>
		{
			auto a = padTo32Bytes(readMemSlot(offA, _loc), _loc);
			auto b = padTo32Bytes(readMemSlot(offB, _loc), _loc);
			auto c = std::make_shared<awst::IntrinsicCall>();
			c->sourceLocation = _loc;
			c->wtype = awst::WType::bytesType();
			c->opCode = "concat";
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
			auto g2 = std::make_shared<awst::IntrinsicCall>();
			g2->sourceLocation = _loc;
			g2->wtype = awst::WType::bytesType();
			g2->opCode = "concat";
			g2->stackArgs.push_back(std::move(g2_x));
			g2->stackArgs.push_back(std::move(g2_y));

			if (!g1Points)
				g1Points = std::move(g1);
			else
			{
				auto c = std::make_shared<awst::IntrinsicCall>();
				c->sourceLocation = _loc;
				c->wtype = awst::WType::bytesType();
				c->opCode = "concat";
				c->stackArgs.push_back(std::move(g1Points));
				c->stackArgs.push_back(std::move(g1));
				g1Points = std::move(c);
			}
			if (!g2Points)
				g2Points = std::move(g2);
			else
			{
				auto c = std::make_shared<awst::IntrinsicCall>();
				c->sourceLocation = _loc;
				c->wtype = awst::WType::bytesType();
				c->opCode = "concat";
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

	auto recoveryId = std::make_shared<awst::IntrinsicCall>();
	recoveryId->sourceLocation = _loc;
	recoveryId->wtype = awst::WType::uint64Type();
	recoveryId->opCode = "btoi";
	recoveryId->stackArgs.push_back(std::move(vBytes));

	// 3. Call ecdsa_pk_recover Secp256k1
	// Returns (bytes, bytes) — pubkey_x and pubkey_y, each 32 bytes
	awst::WType const* tupleTypePtr = m_typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()}
	);

	auto ecdsaRecover = std::make_shared<awst::IntrinsicCall>();
	ecdsaRecover->sourceLocation = _loc;
	ecdsaRecover->wtype = tupleTypePtr;
	ecdsaRecover->opCode = "ecdsa_pk_recover";
	ecdsaRecover->immediates.push_back("Secp256k1");
	ecdsaRecover->stackArgs.push_back(std::move(msgHash));
	ecdsaRecover->stackArgs.push_back(std::move(recoveryId));
	ecdsaRecover->stackArgs.push_back(std::move(r));
	ecdsaRecover->stackArgs.push_back(std::move(s));

	// Store the tuple result in a temporary
	std::string tupleVar = "__ecdsa_result";
	m_locals[tupleVar] = tupleTypePtr;

	auto tupleTarget = awst::makeVarExpression(tupleVar, tupleTypePtr, _loc);

	auto assignTuple = std::make_shared<awst::AssignmentStatement>();
	assignTuple->sourceLocation = _loc;
	assignTuple->target = tupleTarget;
	assignTuple->value = std::move(ecdsaRecover);
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
	auto pubkeyConcat = std::make_shared<awst::IntrinsicCall>();
	pubkeyConcat->sourceLocation = _loc;
	pubkeyConcat->wtype = awst::WType::bytesType();
	pubkeyConcat->opCode = "concat";
	pubkeyConcat->stackArgs.push_back(std::move(pubkeyX));
	pubkeyConcat->stackArgs.push_back(std::move(pubkeyY));

	// 6. keccak256(concat) → 32 bytes
	auto hash = std::make_shared<awst::IntrinsicCall>();
	hash->sourceLocation = _loc;
	hash->wtype = awst::WType::bytesType();
	hash->opCode = "keccak256";
	hash->stackArgs.push_back(std::move(pubkeyConcat));

	// 7. extract3(hash, 12, 20) → last 20 bytes (Ethereum address)
	auto off12 = awst::makeIntegerConstant("12", _loc);
	auto len20 = awst::makeIntegerConstant("20", _loc);

	auto addr = std::make_shared<awst::IntrinsicCall>();
	addr->sourceLocation = _loc;
	addr->wtype = awst::WType::bytesType();
	addr->opCode = "extract3";
	addr->stackArgs.push_back(std::move(hash));
	addr->stackArgs.push_back(std::move(off12));
	addr->stackArgs.push_back(std::move(len20));

	// 8. Left-pad to 32 bytes: concat(bzero(12), addr)
	auto pad12 = std::make_shared<awst::IntrinsicCall>();
	pad12->sourceLocation = _loc;
	pad12->wtype = awst::WType::bytesType();
	pad12->opCode = "bzero";
	auto twelve = awst::makeIntegerConstant("12", _loc);
	pad12->stackArgs.push_back(std::move(twelve));

	auto paddedAddr = std::make_shared<awst::IntrinsicCall>();
	paddedAddr->sourceLocation = _loc;
	paddedAddr->wtype = awst::WType::bytesType();
	paddedAddr->opCode = "concat";
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

		auto truncated = std::make_shared<awst::IntrinsicCall>();
		truncated->sourceLocation = _loc;
		truncated->wtype = awst::WType::bytesType();
		truncated->opCode = "extract3";
		truncated->stackArgs.push_back(std::move(partialSlot));
		truncated->stackArgs.push_back(std::move(offZero));
		truncated->stackArgs.push_back(std::move(remLen));

		if (!inputData)
			inputData = std::move(truncated);
		else
		{
			auto concat = std::make_shared<awst::IntrinsicCall>();
			concat->sourceLocation = _loc;
			concat->wtype = awst::WType::bytesType();
			concat->opCode = "concat";
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
	auto sha256Call = std::make_shared<awst::IntrinsicCall>();
	sha256Call->sourceLocation = _loc;
	sha256Call->wtype = awst::WType::bytesType();
	sha256Call->opCode = "sha256";
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
		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = makeVar(target);
		assign->value = std::move(value);
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

	auto extractData = std::make_shared<awst::IntrinsicCall>();
	extractData->sourceLocation = _loc;
	extractData->wtype = awst::WType::bytesType();
	extractData->opCode = "extract3";
	extractData->stackArgs.push_back(memoryVar(_loc));
	extractData->stackArgs.push_back(std::move(inOffConst));
	extractData->stackArgs.push_back(std::move(inSizeConst));

	auto outOffConst = awst::makeIntegerConstant(std::to_string(_outputOffset), _loc);

	auto replace = std::make_shared<awst::IntrinsicCall>();
	replace->sourceLocation = _loc;
	replace->wtype = awst::WType::bytesType();
	replace->opCode = "replace3";
	replace->stackArgs.push_back(memoryVar(_loc));
	replace->stackArgs.push_back(std::move(outOffConst));
	replace->stackArgs.push_back(std::move(extractData));

	assignMemoryVar(std::move(replace), _loc, _out);
}


} // namespace puyasol::builder
