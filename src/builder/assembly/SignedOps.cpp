/// @file SignedOps.cpp
/// Signed arithmetic: sdiv, smod, slt, sgt, sar, tload, tstore, isNegative256, negate256.

#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageLayout.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::handleTload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// tload(slot) → extract 32 bytes from __transient blob at slot*32
	if (_args.empty()) return nullptr;

	auto slot = ensureBiguint(_args[0], _loc);

	// Convert slot to uint64 offset: slot * 32
	auto slotBytes = awst::makeReinterpretCast(std::move(slot), awst::WType::bytesType(), _loc);
	auto slotU64 = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
	slotU64->stackArgs.push_back(std::move(slotBytes));

	auto thirtyTwo = awst::makeIntegerConstant("32", _loc);

	auto offset = awst::makeUInt64BinOp(std::move(slotU64), awst::UInt64BinaryOperator::Mult, std::move(thirtyTwo), _loc);

	// extract3(__transient, offset, 32)
	auto blob = awst::makeVarExpression("__transient", awst::WType::bytesType(), _loc);

	auto thirtyTwo2 = awst::makeIntegerConstant("32", _loc);

	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(std::move(blob));
	extract->stackArgs.push_back(std::move(offset));
	extract->stackArgs.push_back(std::move(thirtyTwo2));

	// Reinterpret as biguint
	auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
	return cast;
}

void AssemblyBuilder::handleTstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// tstore(slot, value) → replace 32 bytes in __transient blob at slot*32
	if (_args.size() < 2) return;

	auto slot = ensureBiguint(_args[0], _loc);
	auto value = ensureBiguint(_args[1], _loc);

	// Convert slot to uint64 offset: slot * 32
	auto slotBytes = awst::makeReinterpretCast(std::move(slot), awst::WType::bytesType(), _loc);
	auto slotU64 = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
	slotU64->stackArgs.push_back(std::move(slotBytes));

	auto thirtyTwo = awst::makeIntegerConstant("32", _loc);

	auto offset = awst::makeUInt64BinOp(std::move(slotU64), awst::UInt64BinaryOperator::Mult, std::move(thirtyTwo), _loc);

	// Convert value to 32 bytes: b| with bzero(32)
	auto valueBytes = awst::makeReinterpretCast(std::move(value), awst::WType::bytesType(), _loc);

	auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	auto sz = awst::makeIntegerConstant("32", _loc);
	zeros->stackArgs.push_back(std::move(sz));

	auto padded = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), _loc);
	padded->stackArgs.push_back(std::move(zeros));
	padded->stackArgs.push_back(std::move(valueBytes));

	// replace3(__transient, offset, padded_value)
	auto blobRead = awst::makeVarExpression("__transient", awst::WType::bytesType(), _loc);

	auto replace = awst::makeIntrinsicCall("replace3", awst::WType::bytesType(), _loc);
	replace->stackArgs.push_back(std::move(blobRead));
	replace->stackArgs.push_back(std::move(offset));
	replace->stackArgs.push_back(std::move(padded));

	// __transient = replace3(...)
	auto target = awst::makeVarExpression("__transient", awst::WType::bytesType(), _loc);

	auto assign = awst::makeAssignmentStatement(std::move(target), std::move(replace), _loc);
	_out.push_back(std::move(assign));
}

// ─── Signed integer helpers ──────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::isNegative256(
	std::shared_ptr<awst::Expression> _val,
	awst::SourceLocation const& _loc,
	awst::WType const* _origType
)
{
	// Check sign bit in two's complement.
	// For biguint (256-bit): sign bit at position 255, threshold = 2^255
	// For uint64 (64-bit): sign bit at position 63, threshold = 2^63
	// This matters when uint64 variables hold two's complement values after
	// coercion back from biguint (e.g., signextend result coerced to uint64).
	auto halfMax = awst::makeIntegerConstant(
		_origType && _origType == awst::WType::uint64Type()
			? "9223372036854775808" // 2^63
			: "57896044618658097711785492504343953926634992332820282019728792003956564819968", // 2^255
		_loc, awst::WType::biguintType());

	auto cmp = awst::makeNumericCompare(_val, awst::NumericComparison::Gte, std::move(halfMax), _loc);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::negate256(
	std::shared_ptr<awst::Expression> _val,
	awst::SourceLocation const& _loc
)
{
	// Two's complement negate: (~val + 1) mod 2^256
	// Equivalent: (2^256 - val) mod 2^256

	// For biguint, we do: MAX_UINT256 - val + 1
	auto maxU256 = awst::makeIntegerConstant(
		"115792089237316195423570985008687907853269984665640564039457584007913129639935", // 2^256 - 1
		_loc, awst::WType::biguintType());

	auto sub = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, _val, _loc);

	auto one = awst::makeIntegerConstant("1", _loc, awst::WType::biguintType());

	return makeBigUIntBinOp(sub, awst::BigUIntBinaryOperator::Add, one, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSdiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sdiv(a, b) — signed division in two's complement
	// Sign of result = sign of a XOR sign of b
	// |result| = |a| / |b|
	// If b == 0, result = 0 (EVM convention)
	if (_args.size() != 2)
	{
		Logger::instance().error("sdiv requires 2 arguments", _loc);
		return nullptr;
	}

	// Ensure args are biguint (may be uint64 or bytes from other ops)
	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	auto aNeg = isNegative256(a, _loc);
	auto bNeg = isNegative256(b, _loc);

	// |a| = aNeg ? negate(a) : a
	auto absA = std::make_shared<awst::ConditionalExpression>();
	absA->sourceLocation = _loc;
	absA->wtype = awst::WType::biguintType();
	absA->condition = aNeg;
	absA->trueExpr = negate256(a, _loc);
	absA->falseExpr = a;

	// |b| = bNeg ? negate(b) : b
	auto absB = std::make_shared<awst::ConditionalExpression>();
	absB->sourceLocation = _loc;
	absB->wtype = awst::WType::biguintType();
	absB->condition = bNeg;
	absB->trueExpr = negate256(b, _loc);
	absB->falseExpr = b;

	// |a| / |b|
	auto quotient = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::FloorDiv, absB, _loc);

	// resultNeg = aNeg XOR bNeg
	auto aNeg2 = isNegative256(a, _loc);
	auto bNeg2 = isNegative256(b, _loc);
	auto aNegInt = ensureBiguint(aNeg2, _loc);
	auto bNegInt = ensureBiguint(bNeg2, _loc);
	auto xorResult = awst::makeNumericCompare(aNegInt, awst::NumericComparison::Ne, bNegInt, _loc);

	// result = resultNeg ? negate(quotient) : quotient
	auto result = std::make_shared<awst::ConditionalExpression>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->condition = std::move(xorResult);
	result->trueExpr = negate256(quotient, _loc);
	result->falseExpr = quotient;
	return result;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// smod(a, b) — signed modulo: sign of result = sign of a
	// |result| = |a| % |b|
	if (_args.size() != 2)
	{
		Logger::instance().error("smod requires 2 arguments", _loc);
		return nullptr;
	}

	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	auto aNeg = isNegative256(a, _loc);

	// |a|
	auto absA = std::make_shared<awst::ConditionalExpression>();
	absA->sourceLocation = _loc;
	absA->wtype = awst::WType::biguintType();
	absA->condition = aNeg;
	absA->trueExpr = negate256(a, _loc);
	absA->falseExpr = a;

	// |b|
	auto bNeg = isNegative256(b, _loc);
	auto absB = std::make_shared<awst::ConditionalExpression>();
	absB->sourceLocation = _loc;
	absB->wtype = awst::WType::biguintType();
	absB->condition = bNeg;
	absB->trueExpr = negate256(b, _loc);
	absB->falseExpr = b;

	// |a| % |b|
	auto remainder = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::Mod, absB, _loc);

	// result = aNeg ? negate(remainder) : remainder
	auto aNeg2 = isNegative256(a, _loc);
	auto result = std::make_shared<awst::ConditionalExpression>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->condition = std::move(aNeg2);
	result->trueExpr = negate256(remainder, _loc);
	result->falseExpr = remainder;
	return result;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSlt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// slt(a, b) — signed less-than in two's complement
	if (_args.size() != 2)
	{
		Logger::instance().error("slt requires 2 arguments", _loc);
		return nullptr;
	}

	// Capture original types before ensureBiguint conversion, so we can use
	// the correct sign-bit threshold (bit 63 for uint64, bit 255 for biguint).
	auto const* origTypeA = _args[0]->wtype;
	auto const* origTypeB = _args[1]->wtype;
	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	// Special case: slt(x, 0) = isNegative(x)
	// The general case uses ConditionalExpression with (a < b) unsigned, which
	// puya's optimizer constant-folds to false when b=0 (no unsigned biguint < 0),
	// collapsing the entire slt expression. Emit the sign-bit check directly.
	if (auto* bConst = dynamic_cast<awst::IntegerConstant*>(b.get()))
	{
		if (bConst->value == "0")
		{
			return ensureBiguint(isNegative256(a, _loc, origTypeA), _loc);
		}
	}

	// Special case: slt(0, x) = x > 0 && x < signBitThreshold (positive non-zero)
	if (auto* aConst = dynamic_cast<awst::IntegerConstant*>(a.get()))
	{
		if (aConst->value == "0")
		{
			auto zero = awst::makeIntegerConstant("0", _loc, awst::WType::biguintType());
			auto signThreshold = awst::makeIntegerConstant(
				origTypeB && origTypeB == awst::WType::uint64Type()
					? "9223372036854775808" // 2^63
					: "57896044618658097711785492504343953926634992332820282019728792003956564819968", // 2^255
				_loc, awst::WType::biguintType());
			// x > 0
			auto gtZero = awst::makeNumericCompare(b, awst::NumericComparison::Gt, std::move(zero), _loc);
			// x < signBitThreshold
			auto ltPow = awst::makeNumericCompare(b, awst::NumericComparison::Lt, std::move(signThreshold), _loc);
			// AND
			auto andExpr = std::make_shared<awst::BooleanBinaryOperation>();
			andExpr->sourceLocation = _loc;
			andExpr->wtype = awst::WType::boolType();
			andExpr->left = std::move(gtZero);
			andExpr->op = awst::BinaryBooleanOperator::And;
			andExpr->right = std::move(ltPow);
			return ensureBiguint(andExpr, _loc);
		}
	}

	// General case: compare signs, then unsigned comparison
	auto aNeg = isNegative256(a, _loc, origTypeA);
	auto bNeg = isNegative256(b, _loc, origTypeB);
	auto aNeg2 = isNegative256(a, _loc, origTypeA);

	// signsMatch = (aNeg == bNeg) via biguint comparison
	auto aNegInt = ensureBiguint(aNeg, _loc);
	auto bNegInt = ensureBiguint(bNeg, _loc);
	auto signsMatch = awst::makeNumericCompare(aNegInt, awst::NumericComparison::Eq, bNegInt, _loc);

	// unsignedLt = a < b
	auto unsignedLt = awst::makeNumericCompare(a, awst::NumericComparison::Lt, b, _loc);

	// signsMatch ? (a < b) : aNeg
	auto result = std::make_shared<awst::ConditionalExpression>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::boolType();
	result->condition = signsMatch;
	result->trueExpr = unsignedLt;
	result->falseExpr = aNeg2;

	// Convert bool to biguint (Yul semantics: slt returns 0 or 1 as uint256)
	return ensureBiguint(result, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSgt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sgt(a, b) = slt(b, a) — just swap arguments
	if (_args.size() != 2)
	{
		Logger::instance().error("sgt requires 2 arguments", _loc);
		return nullptr;
	}
	std::vector<std::shared_ptr<awst::Expression>> swapped = {_args[1], _args[0]};
	return handleSlt(swapped, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSar(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sar(shift, value) — arithmetic right shift (preserves sign)
	// If value is positive: same as shr(shift, value)
	// If value is negative: shr(shift, value) | ~(shr(shift, MAX_UINT256))
	//   i.e., fill shifted-in bits with 1s instead of 0s
	//
	// Simpler: for positive, shr works. For negative, negate → shr → negate.
	// Actually: sar(n, x) for negative x = ~(~x >> n) = negate(shr(n, negate(x) - 1)) - 1
	// That's complex. Let's use the conditional approach:
	//
	// If not negative: result = shr(shift, value) = value / 2^shift
	// If negative: result = (value / 2^shift) | ((2^256 - 1) - (2^(256-shift) - 1))
	//            = (value / 2^shift) | (mask with top `shift` bits set)
	//
	// Easiest correct implementation:
	// isNeg = value >= 2^255
	// shr_result = value / 2^shift
	// If isNeg: fill = (2^256 - 1) << (256 - shift) [all ones in top shift bits]
	//         = (2^256 - 1) - (2^(256-shift) - 1) = 2^256 - 2^(256-shift)
	//   result = shr_result | fill
	// Else: result = shr_result

	if (_args.size() != 2)
	{
		Logger::instance().error("sar requires 2 arguments", _loc);
		return nullptr;
	}

	// Ensure value is biguint for sign checking
	auto val = ensureBiguint(_args[1], _loc);
	std::vector<std::shared_ptr<awst::Expression>> coercedArgs = {_args[0], val};

	// shr_result = value / 2^shift
	auto shrResult = handleShr(coercedArgs, _loc);

	auto valNeg = isNegative256(val, _loc);

	// For negative case: fill top bits with 1s
	// fillMask = MAX_UINT256 * 2^(256 - shift) mod 2^256
	// Simpler: fillMask = MAX_UINT256 - (2^(256 - shift) - 1) = MAX_UINT256 - 2^(256-shift) + 1
	// Or: ~(2^(256-shift) - 1) in 256 bits

	// We need (256 - shift) as uint64
	auto shiftAmt = _args[0];
	std::shared_ptr<awst::Expression> shiftU64;
	if (shiftAmt->wtype != awst::WType::uint64Type())
		shiftU64 = safeBtoi(shiftAmt, _loc);
	else
		shiftU64 = shiftAmt;

	auto twoFiftySix = awst::makeIntegerConstant("256", _loc);

	auto complementShift = awst::makeUInt64BinOp(std::move(twoFiftySix), awst::UInt64BinaryOperator::Sub, std::move(shiftU64), _loc);

	auto pow2Complement = buildPowerOf2(complementShift, _loc);

	// fillMask = MAX_UINT256 - pow2Complement + 1
	auto maxU256 = awst::makeIntegerConstant("115792089237316195423570985008687907853269984665640564039457584007913129639935", _loc, awst::WType::biguintType());

	auto sub1 = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, pow2Complement, _loc);

	auto one = awst::makeIntegerConstant("1", _loc, awst::WType::biguintType());

	auto fillMask = makeBigUIntBinOp(sub1, awst::BigUIntBinaryOperator::Add, one, _loc);

	// negResult = shr_result | fillMask (using b|)
	auto shrBytes = awst::makeReinterpretCast(shrResult, awst::WType::bytesType(), _loc);

	auto fillBytes = awst::makeReinterpretCast(fillMask, awst::WType::bytesType(), _loc);

	auto orCall = awst::makeIntrinsicCall("b|", awst::WType::bytesType(), _loc);
	orCall->stackArgs.push_back(std::move(shrBytes));
	orCall->stackArgs.push_back(std::move(fillBytes));

	auto negResult = awst::makeReinterpretCast(std::move(orCall), awst::WType::biguintType(), _loc);

	// posResult = shr (re-compute to avoid sharing)
	auto posResult = handleShr(coercedArgs, _loc);

	auto result = std::make_shared<awst::ConditionalExpression>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->condition = valNeg;
	result->trueExpr = negResult;
	result->falseExpr = posResult;
	return result;
}

void AssemblyBuilder::handleSstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("sstore requires 2 arguments", _loc);
		return;
	}

	// Convert slot arg to uint64 for __storage_write(slot, value)
	auto slotArg = _args[0];
	if (slotArg->wtype == awst::WType::biguintType())
		slotArg = safeBtoi(std::move(slotArg), _loc);

	// Ensure value is biguint
	auto valueArg = ensureBiguint(_args[1], _loc);

	// Call __storage_write(slot, value)
	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::voidType();
	call->target = awst::InstanceMethodTarget{"__storage_write"};

	awst::CallArg slotCA;
	slotCA.name = "__slot";
	slotCA.value = std::move(slotArg);
	call->args.push_back(std::move(slotCA));

	awst::CallArg valCA;
	valCA.name = "__value";
	valCA.value = std::move(valueArg);
	call->args.push_back(std::move(valCA));

	auto stmt = awst::makeExpressionStatement(std::move(call), _loc);
	_out.push_back(std::move(stmt));
}


} // namespace puyasol::builder
