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
	auto slotBytes = std::make_shared<awst::ReinterpretCast>();
	slotBytes->sourceLocation = _loc;
	slotBytes->wtype = awst::WType::bytesType();
	slotBytes->expr = std::move(slot);
	auto slotU64 = std::make_shared<awst::IntrinsicCall>();
	slotU64->sourceLocation = _loc;
	slotU64->wtype = awst::WType::uint64Type();
	slotU64->opCode = "btoi";
	slotU64->stackArgs.push_back(std::move(slotBytes));

	auto thirtyTwo = std::make_shared<awst::IntegerConstant>();
	thirtyTwo->sourceLocation = _loc;
	thirtyTwo->wtype = awst::WType::uint64Type();
	thirtyTwo->value = "32";

	auto offset = std::make_shared<awst::UInt64BinaryOperation>();
	offset->sourceLocation = _loc;
	offset->wtype = awst::WType::uint64Type();
	offset->left = std::move(slotU64);
	offset->op = awst::UInt64BinaryOperator::Mult;
	offset->right = std::move(thirtyTwo);

	// extract3(__transient, offset, 32)
	auto blob = std::make_shared<awst::VarExpression>();
	blob->sourceLocation = _loc;
	blob->name = "__transient";
	blob->wtype = awst::WType::bytesType();

	auto thirtyTwo2 = std::make_shared<awst::IntegerConstant>();
	thirtyTwo2->sourceLocation = _loc;
	thirtyTwo2->wtype = awst::WType::uint64Type();
	thirtyTwo2->value = "32";

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(blob));
	extract->stackArgs.push_back(std::move(offset));
	extract->stackArgs.push_back(std::move(thirtyTwo2));

	// Reinterpret as biguint
	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = awst::WType::biguintType();
	cast->expr = std::move(extract);
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
	auto slotBytes = std::make_shared<awst::ReinterpretCast>();
	slotBytes->sourceLocation = _loc;
	slotBytes->wtype = awst::WType::bytesType();
	slotBytes->expr = std::move(slot);
	auto slotU64 = std::make_shared<awst::IntrinsicCall>();
	slotU64->sourceLocation = _loc;
	slotU64->wtype = awst::WType::uint64Type();
	slotU64->opCode = "btoi";
	slotU64->stackArgs.push_back(std::move(slotBytes));

	auto thirtyTwo = std::make_shared<awst::IntegerConstant>();
	thirtyTwo->sourceLocation = _loc;
	thirtyTwo->wtype = awst::WType::uint64Type();
	thirtyTwo->value = "32";

	auto offset = std::make_shared<awst::UInt64BinaryOperation>();
	offset->sourceLocation = _loc;
	offset->wtype = awst::WType::uint64Type();
	offset->left = std::move(slotU64);
	offset->op = awst::UInt64BinaryOperator::Mult;
	offset->right = std::move(thirtyTwo);

	// Convert value to 32 bytes: b| with bzero(32)
	auto valueBytes = std::make_shared<awst::ReinterpretCast>();
	valueBytes->sourceLocation = _loc;
	valueBytes->wtype = awst::WType::bytesType();
	valueBytes->expr = std::move(value);

	auto zeros = std::make_shared<awst::IntrinsicCall>();
	zeros->sourceLocation = _loc;
	zeros->wtype = awst::WType::bytesType();
	zeros->opCode = "bzero";
	auto sz = std::make_shared<awst::IntegerConstant>();
	sz->sourceLocation = _loc;
	sz->wtype = awst::WType::uint64Type();
	sz->value = "32";
	zeros->stackArgs.push_back(std::move(sz));

	auto padded = std::make_shared<awst::IntrinsicCall>();
	padded->sourceLocation = _loc;
	padded->wtype = awst::WType::bytesType();
	padded->opCode = "b|";
	padded->stackArgs.push_back(std::move(zeros));
	padded->stackArgs.push_back(std::move(valueBytes));

	// replace3(__transient, offset, padded_value)
	auto blobRead = std::make_shared<awst::VarExpression>();
	blobRead->sourceLocation = _loc;
	blobRead->name = "__transient";
	blobRead->wtype = awst::WType::bytesType();

	auto replace = std::make_shared<awst::IntrinsicCall>();
	replace->sourceLocation = _loc;
	replace->wtype = awst::WType::bytesType();
	replace->opCode = "replace3";
	replace->stackArgs.push_back(std::move(blobRead));
	replace->stackArgs.push_back(std::move(offset));
	replace->stackArgs.push_back(std::move(padded));

	// __transient = replace3(...)
	auto target = std::make_shared<awst::VarExpression>();
	target->sourceLocation = _loc;
	target->name = "__transient";
	target->wtype = awst::WType::bytesType();

	auto assign = std::make_shared<awst::AssignmentStatement>();
	assign->sourceLocation = _loc;
	assign->target = std::move(target);
	assign->value = std::move(replace);
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
	auto halfMax = std::make_shared<awst::IntegerConstant>();
	halfMax->sourceLocation = _loc;
	halfMax->wtype = awst::WType::biguintType();
	if (_origType && _origType == awst::WType::uint64Type())
		halfMax->value = "9223372036854775808"; // 2^63
	else
		halfMax->value = "57896044618658097711785492504343953926634992332820282019728792003956564819968"; // 2^255

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = _val;
	cmp->op = awst::NumericComparison::Gte;
	cmp->rhs = std::move(halfMax);
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
	auto maxU256 = std::make_shared<awst::IntegerConstant>();
	maxU256->sourceLocation = _loc;
	maxU256->wtype = awst::WType::biguintType();
	maxU256->value = "115792089237316195423570985008687907853269984665640564039457584007913129639935"; // 2^256 - 1

	auto sub = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, _val, _loc);

	auto one = std::make_shared<awst::IntegerConstant>();
	one->sourceLocation = _loc;
	one->wtype = awst::WType::biguintType();
	one->value = "1";

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
	auto xorResult = std::make_shared<awst::NumericComparisonExpression>();
	xorResult->sourceLocation = _loc;
	xorResult->wtype = awst::WType::boolType();
	xorResult->lhs = aNegInt;
	xorResult->op = awst::NumericComparison::Ne;
	xorResult->rhs = bNegInt;

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
			auto zero = std::make_shared<awst::IntegerConstant>();
			zero->sourceLocation = _loc;
			zero->wtype = awst::WType::biguintType();
			zero->value = "0";
			auto signThreshold = std::make_shared<awst::IntegerConstant>();
			signThreshold->sourceLocation = _loc;
			signThreshold->wtype = awst::WType::biguintType();
			if (origTypeB && origTypeB == awst::WType::uint64Type())
				signThreshold->value = "9223372036854775808"; // 2^63
			else
				signThreshold->value = "57896044618658097711785492504343953926634992332820282019728792003956564819968"; // 2^255
			// x > 0
			auto gtZero = std::make_shared<awst::NumericComparisonExpression>();
			gtZero->sourceLocation = _loc;
			gtZero->wtype = awst::WType::boolType();
			gtZero->lhs = b;
			gtZero->op = awst::NumericComparison::Gt;
			gtZero->rhs = std::move(zero);
			// x < signBitThreshold
			auto ltPow = std::make_shared<awst::NumericComparisonExpression>();
			ltPow->sourceLocation = _loc;
			ltPow->wtype = awst::WType::boolType();
			ltPow->lhs = b;
			ltPow->op = awst::NumericComparison::Lt;
			ltPow->rhs = std::move(signThreshold);
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
	auto signsMatch = std::make_shared<awst::NumericComparisonExpression>();
	signsMatch->sourceLocation = _loc;
	signsMatch->wtype = awst::WType::boolType();
	signsMatch->lhs = aNegInt;
	signsMatch->op = awst::NumericComparison::Eq;
	signsMatch->rhs = bNegInt;

	// unsignedLt = a < b
	auto unsignedLt = std::make_shared<awst::NumericComparisonExpression>();
	unsignedLt->sourceLocation = _loc;
	unsignedLt->wtype = awst::WType::boolType();
	unsignedLt->lhs = a;
	unsignedLt->op = awst::NumericComparison::Lt;
	unsignedLt->rhs = b;

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

	auto twoFiftySix = std::make_shared<awst::IntegerConstant>();
	twoFiftySix->sourceLocation = _loc;
	twoFiftySix->wtype = awst::WType::uint64Type();
	twoFiftySix->value = "256";

	auto complementShift = std::make_shared<awst::UInt64BinaryOperation>();
	complementShift->sourceLocation = _loc;
	complementShift->wtype = awst::WType::uint64Type();
	complementShift->left = std::move(twoFiftySix);
	complementShift->right = std::move(shiftU64);
	complementShift->op = awst::UInt64BinaryOperator::Sub;

	auto pow2Complement = buildPowerOf2(complementShift, _loc);

	// fillMask = MAX_UINT256 - pow2Complement + 1
	auto maxU256 = std::make_shared<awst::IntegerConstant>();
	maxU256->sourceLocation = _loc;
	maxU256->wtype = awst::WType::biguintType();
	maxU256->value = "115792089237316195423570985008687907853269984665640564039457584007913129639935";

	auto sub1 = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, pow2Complement, _loc);

	auto one = std::make_shared<awst::IntegerConstant>();
	one->sourceLocation = _loc;
	one->wtype = awst::WType::biguintType();
	one->value = "1";

	auto fillMask = makeBigUIntBinOp(sub1, awst::BigUIntBinaryOperator::Add, one, _loc);

	// negResult = shr_result | fillMask (using b|)
	auto shrBytes = std::make_shared<awst::ReinterpretCast>();
	shrBytes->sourceLocation = _loc;
	shrBytes->wtype = awst::WType::bytesType();
	shrBytes->expr = shrResult;

	auto fillBytes = std::make_shared<awst::ReinterpretCast>();
	fillBytes->sourceLocation = _loc;
	fillBytes->wtype = awst::WType::bytesType();
	fillBytes->expr = fillMask;

	auto orCall = std::make_shared<awst::IntrinsicCall>();
	orCall->sourceLocation = _loc;
	orCall->wtype = awst::WType::bytesType();
	orCall->opCode = "b|";
	orCall->stackArgs.push_back(std::move(shrBytes));
	orCall->stackArgs.push_back(std::move(fillBytes));

	auto negResult = std::make_shared<awst::ReinterpretCast>();
	negResult->sourceLocation = _loc;
	negResult->wtype = awst::WType::biguintType();
	negResult->expr = std::move(orCall);

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

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = std::move(call);
	_out.push_back(std::move(stmt));
}


} // namespace puyasol::builder
