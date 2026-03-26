/// @file BitwiseShiftOps.cpp
/// Bitwise and shift operations: shl, shr, div, byte, signextend, buildPowerOf2.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("sload requires 1 argument", _loc);
		return nullptr;
	}
	// sload has no AVM equivalent — EVM raw storage slot access.
	// Return 0 with a warning.
	Logger::instance().warning("sload() has no AVM equivalent (EVM raw storage), returning 0", _loc);
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";
	return zero;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleGas(
	awst::SourceLocation const& _loc
)
{
	// gas() → global OpcodeBudget (uint64) → itob → reinterpret as biguint
	Logger::instance().debug(
		"gas() mapped to AVM OpcodeBudget (analogous but not equivalent to EVM gas)", _loc);
	auto gasCall = std::make_shared<awst::IntrinsicCall>();
	gasCall->sourceLocation = _loc;
	gasCall->wtype = awst::WType::uint64Type();
	gasCall->opCode = "global";
	gasCall->immediates = {std::string("OpcodeBudget")};

	auto itobCall = std::make_shared<awst::IntrinsicCall>();
	itobCall->sourceLocation = _loc;
	itobCall->wtype = awst::WType::bytesType();
	itobCall->opCode = "itob";
	itobCall->stackArgs.push_back(std::move(gasCall));

	auto biguintCast = std::make_shared<awst::ReinterpretCast>();
	biguintCast->sourceLocation = _loc;
	biguintCast->wtype = awst::WType::biguintType();
	biguintCast->expr = std::move(itobCall);
	return biguintCast;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleTimestamp(
	awst::SourceLocation const& _loc
)
{
	// timestamp() → global LatestTimestamp (uint64) → itob → reinterpret as biguint
	auto tsCall = std::make_shared<awst::IntrinsicCall>();
	tsCall->sourceLocation = _loc;
	tsCall->wtype = awst::WType::uint64Type();
	tsCall->opCode = "global";
	tsCall->immediates.push_back("LatestTimestamp");

	auto itobCall = std::make_shared<awst::IntrinsicCall>();
	itobCall->sourceLocation = _loc;
	itobCall->wtype = awst::WType::bytesType();
	itobCall->opCode = "itob";
	itobCall->stackArgs.push_back(std::move(tsCall));

	auto biguintCast = std::make_shared<awst::ReinterpretCast>();
	biguintCast->sourceLocation = _loc;
	biguintCast->wtype = awst::WType::biguintType();
	biguintCast->expr = std::move(itobCall);
	return biguintCast;
}

// ─── New Yul builtins for Uniswap V4 ────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::buildPowerOf2(
	std::shared_ptr<awst::Expression> _shift,
	awst::SourceLocation const& _loc
)
{
	// Construct 2^shift using setbit(bzero(32), 255-shift, 1)
	// since AVM has no bexp opcode
	auto shiftAmt = _shift;

	// Convert to uint64 if needed (shift amount must be uint64 for subtraction)
	if (shiftAmt->wtype != awst::WType::uint64Type())
	{
		// Cast biguint → bytes first
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::bytesType();
		cast->expr = std::move(shiftAmt);

		// Safe btoi: extract last 8 bytes to avoid btoi overflow (> 8 bytes fails)
		// Pattern: concat(bzero(8), bytes) → extract3(result, len-8, 8) → btoi
		auto eight = std::make_shared<awst::IntegerConstant>();
		eight->sourceLocation = _loc;
		eight->wtype = awst::WType::uint64Type();
		eight->value = "8";

		auto bzeroCall = std::make_shared<awst::IntrinsicCall>();
		bzeroCall->sourceLocation = _loc;
		bzeroCall->wtype = awst::WType::bytesType();
		bzeroCall->opCode = "bzero";
		bzeroCall->stackArgs.push_back(eight);

		auto cat = std::make_shared<awst::IntrinsicCall>();
		cat->sourceLocation = _loc;
		cat->wtype = awst::WType::bytesType();
		cat->opCode = "concat";
		cat->stackArgs.push_back(std::move(bzeroCall));
		cat->stackArgs.push_back(std::move(cast));

		auto lenCall = std::make_shared<awst::IntrinsicCall>();
		lenCall->sourceLocation = _loc;
		lenCall->wtype = awst::WType::uint64Type();
		lenCall->opCode = "len";
		lenCall->stackArgs.push_back(cat);

		auto eight2 = std::make_shared<awst::IntegerConstant>();
		eight2->sourceLocation = _loc;
		eight2->wtype = awst::WType::uint64Type();
		eight2->value = "8";

		auto start = std::make_shared<awst::IntrinsicCall>();
		start->sourceLocation = _loc;
		start->wtype = awst::WType::uint64Type();
		start->opCode = "-";
		start->stackArgs.push_back(std::move(lenCall));
		start->stackArgs.push_back(eight2);

		auto eight3 = std::make_shared<awst::IntegerConstant>();
		eight3->sourceLocation = _loc;
		eight3->wtype = awst::WType::uint64Type();
		eight3->value = "8";

		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = _loc;
		extract->wtype = awst::WType::bytesType();
		extract->opCode = "extract3";
		extract->stackArgs.push_back(cat);
		extract->stackArgs.push_back(std::move(start));
		extract->stackArgs.push_back(eight3);

		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = _loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(extract));
		shiftAmt = std::move(btoi);
	}

	// bzero(32) — 256-bit zero buffer
	auto thirtyTwo = std::make_shared<awst::IntegerConstant>();
	thirtyTwo->sourceLocation = _loc;
	thirtyTwo->wtype = awst::WType::uint64Type();
	thirtyTwo->value = "32";

	auto bzero = std::make_shared<awst::IntrinsicCall>();
	bzero->sourceLocation = _loc;
	bzero->wtype = awst::WType::bytesType();
	bzero->opCode = "bzero";
	bzero->stackArgs.push_back(std::move(thirtyTwo));

	// Clamp shift amount to 0..255 — EVM shifts mod 256 implicitly,
	// but puya optimizer may strip wrapMod256 from intermediates
	auto twoFiftySix = std::make_shared<awst::IntegerConstant>();
	twoFiftySix->sourceLocation = _loc;
	twoFiftySix->wtype = awst::WType::uint64Type();
	twoFiftySix->value = "256";

	auto clampedShift = std::make_shared<awst::UInt64BinaryOperation>();
	clampedShift->sourceLocation = _loc;
	clampedShift->wtype = awst::WType::uint64Type();
	clampedShift->left = std::move(shiftAmt);
	clampedShift->right = std::move(twoFiftySix);
	clampedShift->op = awst::UInt64BinaryOperator::Mod;

	// 255 - shift: setbit uses MSB-first ordering, so bit (255-n) = 2^n
	auto twoFiftyFive = std::make_shared<awst::IntegerConstant>();
	twoFiftyFive->sourceLocation = _loc;
	twoFiftyFive->wtype = awst::WType::uint64Type();
	twoFiftyFive->value = "255";

	auto bitIdx = std::make_shared<awst::UInt64BinaryOperation>();
	bitIdx->sourceLocation = _loc;
	bitIdx->wtype = awst::WType::uint64Type();
	bitIdx->left = std::move(twoFiftyFive);
	bitIdx->right = std::move(clampedShift);
	bitIdx->op = awst::UInt64BinaryOperator::Sub;

	// setbit(bzero(32), 255-shift, 1) → bytes with only bit `shift` set
	auto one = std::make_shared<awst::IntegerConstant>();
	one->sourceLocation = _loc;
	one->wtype = awst::WType::uint64Type();
	one->value = "1";

	auto setbit = std::make_shared<awst::IntrinsicCall>();
	setbit->sourceLocation = _loc;
	setbit->wtype = awst::WType::bytesType();
	setbit->opCode = "setbit";
	setbit->stackArgs.push_back(std::move(bzero));
	setbit->stackArgs.push_back(std::move(bitIdx));
	setbit->stackArgs.push_back(std::move(one));

	// Cast bytes → biguint
	auto castToBigUInt = std::make_shared<awst::ReinterpretCast>();
	castToBigUInt->sourceLocation = _loc;
	castToBigUInt->wtype = awst::WType::biguintType();
	castToBigUInt->expr = std::move(setbit);

	return castToBigUInt;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleDiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("div requires 2 arguments", _loc);
		return nullptr;
	}
	// EVM: div(a, 0) = 0. AVM: b/ by 0 panics.
	// Emit: b != 0 ? a / b : 0
	return safeDivMod(
		_args[0], awst::BigUIntBinaryOperator::FloorDiv, _args[1], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleShl(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// shl(shift, value) → value * 2^shift
	// NOTE: Yul shl argument order is (shift, value), NOT (value, shift)
	if (_args.size() != 2)
	{
		Logger::instance().error("shl requires 2 arguments", _loc);
		return nullptr;
	}
	auto power = buildPowerOf2(_args[0], _loc);
	auto product = makeBigUIntBinOp(
		_args[1], awst::BigUIntBinaryOperator::Mult, std::move(power), _loc
	);
	return wrapMod256(std::move(product), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleShr(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// shr(shift, value) → value / 2^shift
	// NOTE: Yul shr argument order is (shift, value), NOT (value, shift)
	if (_args.size() != 2)
	{
		Logger::instance().error("shr requires 2 arguments", _loc);
		return nullptr;
	}
	auto power = buildPowerOf2(_args[0], _loc);
	return makeBigUIntBinOp(
		_args[1], awst::BigUIntBinaryOperator::FloorDiv, std::move(power), _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleByte(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// byte(n, x) → extract byte n from 32-byte big-endian padded x
	// Implementation: pad x to 32 bytes, then extract3(padded, n, 1)
	if (_args.size() != 2)
	{
		Logger::instance().error("byte requires 2 arguments", _loc);
		return nullptr;
	}

	// Pad x to 32 bytes big-endian
	auto padded = padTo32Bytes(_args[1], _loc);

	// Convert n to uint64 for extract3
	auto nExpr = _args[0];
	if (nExpr->wtype != awst::WType::uint64Type())
	{
		nExpr = safeBtoi(std::move(nExpr), _loc);
	}

	// extract3(padded, n, 1)
	auto one = std::make_shared<awst::IntegerConstant>();
	one->sourceLocation = _loc;
	one->wtype = awst::WType::uint64Type();
	one->value = "1";

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(padded));
	extract->stackArgs.push_back(std::move(nExpr));
	extract->stackArgs.push_back(std::move(one));

	// Cast bytes → biguint for Yul semantics (all values are uint256)
	auto castResult = std::make_shared<awst::ReinterpretCast>();
	castResult->sourceLocation = _loc;
	castResult->wtype = awst::WType::biguintType();
	castResult->expr = std::move(extract);
	return castResult;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSignextend(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// signextend(b, x) — sign-extend x from byte b (0-indexed from low byte).
	// If bit 7 of byte b is set, fill higher bytes with 0xFF, else 0x00.
	//
	// Implementation for two's complement in 256-bit biguint:
	//   bitPos = (b + 1) * 8  (total bits that are significant)
	//   signBit = (x >> (bitPos - 1)) & 1
	//   if signBit:
	//     mask = (2^256 - 1) - (2^bitPos - 1)  // all ones above bitPos
	//     result = x | mask
	//   else:
	//     mask = 2^bitPos - 1   // keep only lower bitPos bits
	//     result = x & mask
	//
	// For simplicity and since V4 uses signextend only in specific patterns,
	// we implement the full logic using conditional expression.
	if (_args.size() != 2)
	{
		Logger::instance().error("signextend requires 2 arguments", _loc);
		return nullptr;
	}

	// Ensure x is biguint
	auto x = ensureBiguint(_args[1], _loc);

	// For constant b values, we can optimize
	auto bConst = resolveConstantOffset(_args[0]);
	if (!bConst.has_value())
	{
		Logger::instance().warning("signextend with non-constant b, returning x unchanged", _loc);
		return x;
	}

	uint64_t b = *bConst;
	if (b >= 31)
	{
		// signextend from byte 31 or higher = no-op for 256-bit values
		return x;
	}

	uint64_t bitPos = (b + 1) * 8;

	// Build: signBit = (x >> (bitPos - 1)) & 1
	// Using shr pattern: x / 2^(bitPos-1) mod 2
	auto shiftAmt = std::make_shared<awst::IntegerConstant>();
	shiftAmt->sourceLocation = _loc;
	shiftAmt->wtype = awst::WType::biguintType();
	shiftAmt->value = "1"; // 2^(bitPos-1) via pow2

	auto shiftConst = std::make_shared<awst::IntegerConstant>();
	shiftConst->sourceLocation = _loc;
	shiftConst->wtype = awst::WType::uint64Type();
	shiftConst->value = std::to_string(bitPos - 1);

	auto pow2shift = buildPowerOf2(std::move(shiftConst), _loc);

	// x / 2^(bitPos-1)
	auto shifted = makeBigUIntBinOp(x, awst::BigUIntBinaryOperator::FloorDiv, pow2shift, _loc);

	// ... mod 2
	auto two = std::make_shared<awst::IntegerConstant>();
	two->sourceLocation = _loc;
	two->wtype = awst::WType::biguintType();
	two->value = "2";

	auto signBit = makeBigUIntBinOp(shifted, awst::BigUIntBinaryOperator::Mod, two, _loc);

	// signBit != 0  (i.e., sign bit is set)
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";

	auto isNeg = std::make_shared<awst::NumericComparisonExpression>();
	isNeg->sourceLocation = _loc;
	isNeg->wtype = awst::WType::boolType();
	isNeg->lhs = signBit;
	isNeg->op = awst::NumericComparison::Ne;
	isNeg->rhs = zero;

	// lowMask = 2^bitPos - 1
	auto bitPosConst = std::make_shared<awst::IntegerConstant>();
	bitPosConst->sourceLocation = _loc;
	bitPosConst->wtype = awst::WType::uint64Type();
	bitPosConst->value = std::to_string(bitPos);

	auto pow2BitPos = buildPowerOf2(std::move(bitPosConst), _loc);

	auto oneBI = std::make_shared<awst::IntegerConstant>();
	oneBI->sourceLocation = _loc;
	oneBI->wtype = awst::WType::biguintType();
	oneBI->value = "1";

	auto lowMask = makeBigUIntBinOp(pow2BitPos, awst::BigUIntBinaryOperator::Sub, oneBI, _loc);

	// highMask = ~lowMask in 256 bits = (2^256 - 1) - lowMask
	// Use MAX_UINT256 = 2^256 - 1
	auto maxU256 = std::make_shared<awst::IntegerConstant>();
	maxU256->sourceLocation = _loc;
	maxU256->wtype = awst::WType::biguintType();
	maxU256->value = "115792089237316195423570985008687907853269984665640564039457584007913129639935";

	auto highMask = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, lowMask, _loc);

	// Negative case: x | highMask (set all bits above bitPos)
	auto xCastNeg = std::make_shared<awst::ReinterpretCast>();
	xCastNeg->sourceLocation = _loc;
	xCastNeg->wtype = awst::WType::bytesType();
	xCastNeg->expr = x;

	auto highMaskBytes = std::make_shared<awst::ReinterpretCast>();
	highMaskBytes->sourceLocation = _loc;
	highMaskBytes->wtype = awst::WType::bytesType();
	highMaskBytes->expr = highMask;

	auto orCall = std::make_shared<awst::IntrinsicCall>();
	orCall->sourceLocation = _loc;
	orCall->wtype = awst::WType::bytesType();
	orCall->opCode = "b|";
	orCall->stackArgs.push_back(std::move(xCastNeg));
	orCall->stackArgs.push_back(std::move(highMaskBytes));

	auto negResult = std::make_shared<awst::ReinterpretCast>();
	negResult->sourceLocation = _loc;
	negResult->wtype = awst::WType::biguintType();
	negResult->expr = std::move(orCall);

	// Positive case: x & lowMask (clear all bits above bitPos)
	// Re-create lowMask (can't reuse shared_ptr after move)
	auto bitPosConst2 = std::make_shared<awst::IntegerConstant>();
	bitPosConst2->sourceLocation = _loc;
	bitPosConst2->wtype = awst::WType::uint64Type();
	bitPosConst2->value = std::to_string(bitPos);

	auto pow2BitPos2 = buildPowerOf2(std::move(bitPosConst2), _loc);

	auto oneBI2 = std::make_shared<awst::IntegerConstant>();
	oneBI2->sourceLocation = _loc;
	oneBI2->wtype = awst::WType::biguintType();
	oneBI2->value = "1";

	auto lowMask2 = makeBigUIntBinOp(pow2BitPos2, awst::BigUIntBinaryOperator::Sub, oneBI2, _loc);

	auto xCastPos = std::make_shared<awst::ReinterpretCast>();
	xCastPos->sourceLocation = _loc;
	xCastPos->wtype = awst::WType::bytesType();
	xCastPos->expr = x;

	auto lowMask2Bytes = std::make_shared<awst::ReinterpretCast>();
	lowMask2Bytes->sourceLocation = _loc;
	lowMask2Bytes->wtype = awst::WType::bytesType();
	lowMask2Bytes->expr = lowMask2;

	auto andCall = std::make_shared<awst::IntrinsicCall>();
	andCall->sourceLocation = _loc;
	andCall->wtype = awst::WType::bytesType();
	andCall->opCode = "b&";
	andCall->stackArgs.push_back(std::move(xCastPos));
	andCall->stackArgs.push_back(std::move(lowMask2Bytes));

	auto posResult = std::make_shared<awst::ReinterpretCast>();
	posResult->sourceLocation = _loc;
	posResult->wtype = awst::WType::biguintType();
	posResult->expr = std::move(andCall);

	// Conditional: isNeg ? negResult : posResult
	auto cond = std::make_shared<awst::ConditionalExpression>();
	cond->sourceLocation = _loc;
	cond->wtype = awst::WType::biguintType();
	cond->condition = std::move(isNeg);
	cond->trueExpr = std::move(negResult);
	cond->falseExpr = std::move(posResult);
	return cond;
}


} // namespace puyasol::builder
