/// @file SignedOps.cpp
/// Signed arithmetic: sdiv, smod, slt, sgt, sar, tload, tstore, isNegative256, negate256.

#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h" // makeStateGetWithDefault (box-keyed struct sstore)
#include "builder/sol-types/TypeCoercion.h" // isNegativeSigned (shared sign-bit test)
#include "Logger.h"
#include "builder/proxies/Erc1967Lowering.h"
#include "builder/BuildArtifacts.h"

#include <optional>
#include <sstream>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{


// ─── Signed integer helpers ──────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::isNegative256(
	std::shared_ptr<awst::Expression> _val,
	awst::SourceLocation const& _loc,
	awst::WType const* _origType
)
{
	// value ≥ 2^255 (biguint) or ≥ 2^63 (uint64) indicates a negative two's-complement.
	unsigned bits = (_origType && _origType == awst::WType::uint64Type()) ? 64 : 256;
	return TypeCoercion::isNegativeSigned(std::move(_val), bits, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::negate256(
	std::shared_ptr<awst::Expression> _val,
	awst::SourceLocation const& _loc
)
{
	// Two's complement negate: MAX_UINT256 - val + 1  (= ~val + 1, mod 2^256).
	auto maxU256 = awst::makeIntegerConstant(
		"115792089237316195423570985008687907853269984665640564039457584007913129639935", // 2^256 - 1
		_loc, awst::WType::biguintType());

	auto sub = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, _val, _loc);

	auto one = awst::makeBiguintConstant("1", _loc);

	// Wrap mod 2^256: negate256(0) = (2^256-1)+1 = 2^256 must reduce to 0 (two's
	// complement of 0 is 0), else the out-of-range 2^256 reverts. This is hit
	// whenever a signed asm op (sdiv/smod) produces a zero result with a negative
	// sign, e.g. sdiv(x, int256.min) or smod(int256.min, y) — EVM returns 0.
	// No-op for every val>=1 (2^256-val is already in range).
	auto sum = makeBigUIntBinOp(sub, awst::BigUIntBinaryOperator::Add, one, _loc);
	return wrapMod256(std::move(sum), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSdiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sdiv(a, b): signed division; result sign = sign(a) XOR sign(b). sdiv(a,0)=0.
	if (!checkArity(_args, 2, "sdiv", _loc))
		return nullptr;

	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	auto absA = awst::makeConditional(
		isNegative256(a, _loc), negate256(a, _loc), a, awst::WType::biguintType(), _loc);
	auto absB = awst::makeConditional(
		isNegative256(b, _loc), negate256(b, _loc), b, awst::WType::biguintType(), _loc);
	auto quotient = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::FloorDiv, absB, _loc);

	// resultNeg = aNeg XOR bNeg
	auto xorResult = awst::makeNumericCompare(
		ensureBiguint(isNegative256(a, _loc), _loc), awst::NumericComparison::Ne,
		ensureBiguint(isNegative256(b, _loc), _loc), _loc);
	auto signedResult = awst::makeConditional(
		std::move(xorResult), negate256(quotient, _loc), quotient,
		awst::WType::biguintType(), _loc);

	// b==0 guard: AVM b/ panics; the conditional only evaluates the taken branch.
	auto bNonZero = awst::makeNumericCompare(
		b, awst::NumericComparison::Ne, awst::makeBiguintConstant("0", _loc), _loc);
	return awst::makeConditional(
		std::move(bNonZero), std::move(signedResult),
		awst::makeBiguintConstant("0", _loc), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// smod(a, b): sign of result = sign of a. smod(a,0)=0.
	if (!checkArity(_args, 2, "smod", _loc))
		return nullptr;

	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	auto absA = awst::makeConditional(
		isNegative256(a, _loc), negate256(a, _loc), a, awst::WType::biguintType(), _loc);
	auto absB = awst::makeConditional(
		isNegative256(b, _loc), negate256(b, _loc), b, awst::WType::biguintType(), _loc);
	auto remainder = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::Mod, absB, _loc);
	auto signedResult = awst::makeConditional(
		isNegative256(a, _loc), negate256(remainder, _loc), remainder,
		awst::WType::biguintType(), _loc);

	// b==0 guard: AVM b% panics.
	auto bNonZero = awst::makeNumericCompare(
		b, awst::NumericComparison::Ne, awst::makeBiguintConstant("0", _loc), _loc);
	return awst::makeConditional(
		std::move(bNonZero), std::move(signedResult),
		awst::makeBiguintConstant("0", _loc), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSlt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// slt(a,b): signed less-than (two's complement).
	if (!checkArity(_args, 2, "slt", _loc))
		return nullptr;

	// Capture original types before ensureBiguint: sign-bit threshold differs
	// (bit 63 for uint64, bit 255 for biguint).
	auto const* origTypeA = _args[0]->wtype;
	auto const* origTypeB = _args[1]->wtype;
	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	// slt(x,0) = isNegative(x): avoids puya constant-folding `a<0` to false.
	if (auto* bConst = dynamic_cast<awst::IntegerConstant*>(b.get()))
	{
		if (bConst->value == "0")
		{
			return ensureBiguint(isNegative256(a, _loc, origTypeA), _loc);
		}
	}

	// slt(0,x) = x>0 && x<2^(N-1) (positive non-zero).
	if (auto* aConst = dynamic_cast<awst::IntegerConstant*>(a.get()))
	{
		if (aConst->value == "0")
		{
			auto signThreshold = awst::makeIntegerConstant(
				origTypeB && origTypeB == awst::WType::uint64Type()
					? "9223372036854775808" // 2^63
					: "57896044618658097711785492504343953926634992332820282019728792003956564819968", // 2^255
				_loc, awst::WType::biguintType());
			auto andExpr = awst::makeBoolBinOp(
				awst::makeNumericCompare(b, awst::NumericComparison::Gt,
					awst::makeBiguintConstant("0", _loc), _loc),
				awst::BinaryBooleanOperator::And,
				awst::makeNumericCompare(b, awst::NumericComparison::Lt,
					std::move(signThreshold), _loc), _loc);
			return ensureBiguint(andExpr, _loc);
		}
	}

	// General: signsMatch ? (a < b) : aNeg.
	auto aNeg = isNegative256(a, _loc, origTypeA);
	auto aNeg2 = isNegative256(a, _loc, origTypeA);
	auto signsMatch = awst::makeNumericCompare(
		ensureBiguint(aNeg, _loc), awst::NumericComparison::Eq,
		ensureBiguint(isNegative256(b, _loc, origTypeB), _loc), _loc);
	auto result = awst::makeConditional(
		signsMatch,
		awst::makeNumericCompare(a, awst::NumericComparison::Lt, b, _loc),
		aNeg2, awst::WType::boolType(), _loc);
	return ensureBiguint(result, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSgt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sgt(a,b) = slt(b,a).
	if (!checkArity(_args, 2, "sgt", _loc))
		return nullptr;
	std::vector<std::shared_ptr<awst::Expression>> swapped = {_args[1], _args[0]};
	return handleSlt(swapped, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSar(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sar(shift, value): arithmetic right shift.
	// Positive: result = shr(shift, value).
	// Negative: result = shr | fillMask (top `shift` bits set).
	// fillMask = MAX_UINT256 - 2^(256-shift) + 1.
	// sar(n>=256, x<0) = all-ones: complementShift clamped to 0 →
	// pow2Complement=1 → fillMask=MAX, so 0|MAX=MAX. AVM `-` panics on
	// underflow — the conditional only evaluates (256-shift) when shift<256.
	if (!checkArity(_args, 2, "sar", _loc))
		return nullptr;

	auto val = awst::makeEvalOnce(
		wrapMod256(ensureBiguint(_args[1], _loc), _loc), _loc);
	std::vector<std::shared_ptr<awst::Expression>> coercedArgs = {_args[0], val};
	auto shrResult = handleShr(coercedArgs, _loc);
	auto valNeg = isNegative256(val, _loc);

	// fillMask = the high `shift` sign bits = MAX - shr(shift, MAX). Using shr (which already
	// saturates to 0 for shift>=256 and is identity for shift==0) avoids the 2^256 / underflow edge
	// cases of the old 256-shift complement: at shift==0, shr(0,MAX)=MAX so fillMask=0 (sar(0,x)=x,
	// previously all-ones for negative x); at shift>=256, shr=0 so fillMask=MAX (all sign bits).
	auto maxStr = std::string(
		"115792089237316195423570985008687907853269984665640564039457584007913129639935");
	std::vector<std::shared_ptr<awst::Expression>> shrMaxArgs = {
		_args[0], awst::makeBiguintConstant(maxStr, _loc)};
	auto shrMax = handleShr(shrMaxArgs, _loc);
	auto fillMask = makeBigUIntBinOp(
		awst::makeBiguintConstant(maxStr, _loc), awst::BigUIntBinaryOperator::Sub,
		std::move(shrMax), _loc);

	auto negResult = awst::makeAsBiguint(
		awst::makeBytesOr(awst::makeAsBytes(shrResult, _loc),
			awst::makeAsBytes(fillMask, _loc), _loc), _loc);

	return awst::makeConditional(
		valNeg, negResult, handleShr(coercedArgs, _loc), awst::WType::biguintType(), _loc);
}



} // namespace puyasol::builder
