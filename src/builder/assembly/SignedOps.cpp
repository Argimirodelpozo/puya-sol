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
	// tload(slot) → extract 32 bytes from the transient-storage scratch slot
	// at offset slot*32. The scratch slot is bzero'd in the approval preamble
	// and persists across callsub within an app call, so writes from earlier
	// `this.f()` frames are visible to later tload callers.
	if (_args.empty()) return nullptr;

	auto slot = ensureBiguint(_args[0], _loc);

	// Convert slot to uint64 offset: slot * 32
	auto slotBytes = awst::makeAsBytes(std::move(slot), _loc);
	auto slotU64 = awst::makeBtoi(std::move(slotBytes), _loc);

	auto thirtyTwo = awst::makeIntegerConstant("32", _loc);

	auto offset = awst::makeUInt64BinOp(std::move(slotU64), awst::UInt64BinaryOperator::Mult, std::move(thirtyTwo), _loc);

	// load TRANSIENT_SLOT
	auto loadBlob = awst::makeLoadSlot(TRANSIENT_SLOT, _loc);

	// extract3(blob, offset, 32)
	auto thirtyTwo2 = awst::makeIntegerConstant("32", _loc);

	auto extract = awst::makeExtract3(std::move(loadBlob), std::move(offset), std::move(thirtyTwo2), _loc);
	// Reinterpret as biguint
	auto cast = awst::makeAsBiguint(std::move(extract), _loc);
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
	auto slotBytes = awst::makeAsBytes(std::move(slot), _loc);
	auto slotU64 = awst::makeBtoi(std::move(slotBytes), _loc);

	auto thirtyTwo = awst::makeIntegerConstant("32", _loc);

	auto offset = awst::makeUInt64BinOp(std::move(slotU64), awst::UInt64BinaryOperator::Mult, std::move(thirtyTwo), _loc);

	// Convert value to 32 bytes: zero-extend to at least 32.
	auto valueBytes = awst::makeAsBytes(std::move(value), _loc);

	auto padded = awst::makeZeroExtendToN(std::move(valueBytes), 32, _loc);

	// replace3(load TRANSIENT_SLOT, offset, padded_value)
	auto blobRead = awst::makeLoadSlot(TRANSIENT_SLOT, _loc);

	auto replace = awst::makeReplace3(std::move(blobRead), std::move(offset), std::move(padded), _loc);
	// store TRANSIENT_SLOT ← replace3(...)
	// Direct scratch write: write persists across callsub within the app call,
	// and can't be DCE'd because store is a side-effectful intrinsic.
	auto storeOp = awst::makeStoreSlot(TRANSIENT_SLOT, std::move(replace), _loc);

	auto stmt = awst::makeExpressionStatement(std::move(storeOp), _loc);
	_out.push_back(std::move(stmt));
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

	auto one = awst::makeBiguintConstant("1", _loc);

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
	auto absA = awst::makeConditional(
		aNeg, negate256(a, _loc), a, awst::WType::biguintType(), _loc);

	// |b| = bNeg ? negate(b) : b
	auto absB = awst::makeConditional(
		bNeg, negate256(b, _loc), b, awst::WType::biguintType(), _loc);

	// |a| / |b|
	auto quotient = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::FloorDiv, absB, _loc);

	// resultNeg = aNeg XOR bNeg
	auto aNeg2 = isNegative256(a, _loc);
	auto bNeg2 = isNegative256(b, _loc);
	auto aNegInt = ensureBiguint(aNeg2, _loc);
	auto bNegInt = ensureBiguint(bNeg2, _loc);
	auto xorResult = awst::makeNumericCompare(aNegInt, awst::NumericComparison::Ne, bNegInt, _loc);

	// result = resultNeg ? negate(quotient) : quotient
	return awst::makeConditional(
		std::move(xorResult), negate256(quotient, _loc), quotient,
		awst::WType::biguintType(), _loc);
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
	auto absA = awst::makeConditional(
		aNeg, negate256(a, _loc), a, awst::WType::biguintType(), _loc);

	// |b|
	auto bNeg = isNegative256(b, _loc);
	auto absB = awst::makeConditional(
		bNeg, negate256(b, _loc), b, awst::WType::biguintType(), _loc);

	// |a| % |b|
	auto remainder = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::Mod, absB, _loc);

	// result = aNeg ? negate(remainder) : remainder
	auto aNeg2 = isNegative256(a, _loc);
	return awst::makeConditional(
		std::move(aNeg2), negate256(remainder, _loc), remainder,
		awst::WType::biguintType(), _loc);
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
			auto zero = awst::makeBiguintConstant("0", _loc);
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
			auto andExpr = awst::makeBoolBinOp(std::move(gtZero), awst::BinaryBooleanOperator::And, std::move(ltPow), _loc);
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
	auto result = awst::makeConditional(
		signsMatch, unsignedLt, aNeg2, awst::WType::boolType(), _loc);

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
	auto maxU256 = awst::makeBiguintConstant("115792089237316195423570985008687907853269984665640564039457584007913129639935", _loc);

	auto sub1 = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, pow2Complement, _loc);

	auto one = awst::makeBiguintConstant("1", _loc);

	auto fillMask = makeBigUIntBinOp(sub1, awst::BigUIntBinaryOperator::Add, one, _loc);

	// negResult = shr_result | fillMask (using b|)
	auto shrBytes = awst::makeAsBytes(shrResult, _loc);

	auto fillBytes = awst::makeAsBytes(fillMask, _loc);

	auto orCall = awst::makeBytesOr(std::move(shrBytes), std::move(fillBytes), _loc);

	auto negResult = awst::makeAsBiguint(std::move(orCall), _loc);

	// posResult = shr (re-compute to avoid sharing)
	auto posResult = handleShr(coercedArgs, _loc);

	return awst::makeConditional(
		valNeg, negResult, posResult, awst::WType::biguintType(), _loc);
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
	auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), _loc);

	awst::pushCallArg(call->args, "__slot", std::move(slotArg));
	awst::pushCallArg(call->args, "__value", std::move(valueArg));

	auto stmt = awst::makeExpressionStatement(std::move(call), _loc);
	_out.push_back(std::move(stmt));
}


} // namespace puyasol::builder
