/// @file SignedOps.cpp
/// Signed arithmetic: sdiv, smod, slt, sgt, sar, isNegative256.

#include "builder/yul/AssemblyBuilder.h"
#include "builder/types/TypeCoercion.h" // isNegativeSigned (shared sign-bit test)
#include "builder/eb/BigUIntMathHelpers.h"

namespace puyasol::builder
{


// ─── Signed integer helpers ──────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::isNegative256(
	std::shared_ptr<awst::Expression> _val,
	awst::SourceLocation const& _loc
)
{
	// The carrier is not the Yul word's signedness. Signed Solidity locals
	// are sign-extended at the boundary; unsigned uint64 carriers stay positive.
	return TypeCoercion::isNegativeSigned(ensureBiguint(std::move(_val), _loc), 256, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::buildSignedDivMod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	bool _isDiv, awst::SourceLocation const& _loc
)
{
	// sdiv(a, b) / smod(a, b): |a| op |b|, then re-apply the sign — sign(a) XOR
	// sign(b) for the quotient, sign(a) for the remainder. sdiv(a,0) = smod(a,0) = 0.

	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	auto signedResult = eb::buildSignedModDiv(a, b,
		_isDiv ? eb::BuilderBinaryOp::FloorDiv : eb::BuilderBinaryOp::Mod,
		256, /*checked=*/false, _loc);

	// b==0 guard: AVM b/ and b% panic; the conditional only evaluates the taken branch.
	auto bNonZero = awst::makeNumericCompare(
		b, awst::NumericComparison::Ne, awst::makeBiguintConstant("0", _loc), _loc);
	return awst::makeConditional(
		std::move(bNonZero), std::move(signedResult),
		awst::makeBiguintConstant("0", _loc), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSdiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return buildSignedDivMod(_args, /*_isDiv=*/true, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return buildSignedDivMod(_args, /*_isDiv=*/false, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSlt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// slt(a,b): signed less-than (two's complement).

	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	// slt(x,0) = isNegative(x): avoids puya constant-folding `a<0` to false.
	if (auto* bConst = dynamic_cast<awst::IntegerConstant*>(b.get()))
	{
		if (bConst->value == "0")
		{
			return ensureBiguint(isNegative256(a, _loc), _loc);
		}
	}

	// slt(0,x) = x>0 && x<2^255 (positive non-zero).
	if (auto* aConst = dynamic_cast<awst::IntegerConstant*>(a.get()))
	{
		if (aConst->value == "0")
		{
			auto signThreshold = awst::makeIntegerConstant(
				"57896044618658097711785492504343953926634992332820282019728792003956564819968", // 2^255
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
	auto aNeg = isNegative256(a, _loc);
	auto signsMatch = awst::makeNumericCompare(
		ensureBiguint(aNeg, _loc), awst::NumericComparison::Eq,
		ensureBiguint(isNegative256(b, _loc), _loc), _loc);
	auto result = awst::makeConditional(
		signsMatch,
		awst::makeNumericCompare(a, awst::NumericComparison::Lt, b, _loc),
		aNeg, awst::WType::boolType(), _loc);
	return ensureBiguint(result, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSgt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sgt(a,b) = slt(b,a).
	std::vector<std::shared_ptr<awst::Expression>> swapped = {_args[1], _args[0]};
	return handleSlt(swapped, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSar(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	return eb::buildBigUIntArithmeticShiftRight(
		wrapMod256(ensureBiguint(_args[1], _loc), _loc),
		eb::shiftAmountToUint64(ensureBiguint(_args[0], _loc), _loc), _loc);
}



} // namespace puyasol::builder
