#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-types/TypeCoercion.h"

#include <string>

namespace puyasol::builder::eb
{

std::shared_ptr<awst::Expression> wrapMod256(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	auto pow256 = makePow256(_loc);

	auto mod = awst::makeBigUIntBinOp(std::move(_expr), awst::BigUIntBinaryOperator::Mod, std::move(pow256), _loc);
	return mod;
}

std::shared_ptr<awst::Expression> buildBigUIntShift(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _shiftAmt,
	bool _isLeftShift,
	awst::SourceLocation const& _loc)
{
	// bzero(32)
	auto bzero = awst::makeBzero(32, _loc);

	// 255 - n
	auto twoFiftyFive = awst::makeIntegerConstant("255", _loc);

	auto bitIdx = awst::makeUInt64BinOp(std::move(twoFiftyFive), awst::UInt64BinaryOperator::Sub, std::move(_shiftAmt), _loc);

	// setbit(bzero(32), 255-n, 1)
	auto setbit = awst::makeSetbit(
		std::move(bzero), std::move(bitIdx), awst::makeOne(_loc), _loc);

	auto castPow = awst::makeAsBiguint(std::move(setbit), _loc);

	auto e = awst::makeBigUIntBinOp(std::move(_value), _isLeftShift ? awst::BigUIntBinaryOperator::Mult : awst::BigUIntBinaryOperator::FloorDiv, std::move(castPow), _loc);

	std::shared_ptr<awst::Expression> result = e;

	// Left shift must wrap mod 2^256 (EVM semantics)
	if (_isLeftShift)
		result = wrapMod256(std::move(result), _loc);

	return result;
}

std::shared_ptr<awst::Expression> buildBigUIntExp(
	ContractContext& m_ctx,
	bool _isUnchecked,
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _exp,
	awst::SourceLocation const& _loc)
{
	_base = TypeCoercion::implicitNumericCast(std::move(_base), awst::WType::biguintType(), _loc);
	_exp = TypeCoercion::implicitNumericCast(std::move(_exp), awst::WType::biguintType(), _loc);

	static int expCounter = 0;
	int id = expCounter++;
	std::string resultVar = "__biguint_exp_result_" + std::to_string(id);
	std::string baseVar = "__biguint_exp_base_" + std::to_string(id);
	std::string expVar = "__biguint_exp_exp_" + std::to_string(id);

	auto makeVar = [&](std::string const& name) {
		auto v = awst::makeVarExpression(name, awst::WType::biguintType(), _loc);
		return v;
	};
	auto makeConst = [&](std::string const& value) {
		auto c = awst::makeIntegerConstant(value, _loc, awst::WType::biguintType());
		return c;
	};
	auto makeAssign = [&](std::string const& target, std::shared_ptr<awst::Expression> value) {
		auto a = awst::makeAssignmentStatement(makeVar(target), std::move(value), _loc);
		return a;
	};
	auto makeBinOp = [&](std::shared_ptr<awst::Expression> l, awst::BigUIntBinaryOperator op,
		std::shared_ptr<awst::Expression> r) {
		auto b = awst::makeBigUIntBinOp(std::move(l), op, std::move(r), _loc);
		return b;
	};

	m_ctx.prePendingStatements.push_back(makeAssign(resultVar, makeConst("1")));
	m_ctx.prePendingStatements.push_back(makeAssign(baseVar, std::move(_base)));
	m_ctx.prePendingStatements.push_back(makeAssign(expVar, std::move(_exp)));

	// while exp > 0:
	auto loopCond = awst::makeNumericCompare(makeVar(expVar), awst::NumericComparison::Gt, makeConst("0"), _loc);
	auto body = awst::makeBlock(_loc);

	// In unchecked mode, Solidity wraps intermediate products mod 2^256 so
	// that huge exponents (e.g. 2**1113) don't blow past biguint capacity.
	bool const wrapMod = _isUnchecked;
		auto wrapMod256Inner = [&](std::shared_ptr<awst::Expression> v)
		-> std::shared_ptr<awst::Expression>
	{
		if (!wrapMod) return v;
		return makeBinOp(std::move(v), awst::BigUIntBinaryOperator::Mod, makeConst(kPow2_256));
	};

	// if exp & 1 != 0: result = (result * base) [mod 2^256 if unchecked]
	{
		auto expAnd1 = makeBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::BitAnd, makeConst("1"));
		auto isOdd = awst::makeNumericCompare(std::move(expAnd1), awst::NumericComparison::Ne, makeConst("0"), _loc);

		std::shared_ptr<awst::Expression> product =
			makeBinOp(makeVar(resultVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar));
		product = wrapMod256Inner(std::move(product));

		auto ifBlock = awst::makeBlock(_loc);
		ifBlock->body.push_back(makeAssign(resultVar, std::move(product)));

		body->body.push_back(awst::makeIfElse(
			std::move(isOdd), std::move(ifBlock), nullptr, _loc));
	}

	// exp = exp / 2; base = (base * base) [mod 2^256 if unchecked]
	body->body.push_back(makeAssign(expVar,
		makeBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::FloorDiv, makeConst("2"))));
	{
		std::shared_ptr<awst::Expression> baseSq =
			makeBinOp(makeVar(baseVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar));
		baseSq = wrapMod256Inner(std::move(baseSq));
		body->body.push_back(makeAssign(baseVar, std::move(baseSq)));
	}

	m_ctx.prePendingStatements.push_back(
		awst::makeWhileLoop(std::move(loopCond), std::move(body), _loc));

	return makeVar(resultVar);
}

std::shared_ptr<awst::Expression> buildWrappingSubtract(
	ContractContext& m_ctx,
	bool _isUnchecked,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::SourceLocation const& _loc)
{
	// Checked subtraction: assert a >= b before wrapping
	if (!_isUnchecked)
	{
		auto cmp = awst::makeNumericCompare(_left, awst::NumericComparison::Gte, _right, _loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "underflow"), _loc);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	// (a + 2^256 - b) % 2^256
	auto pow256 = makePow256(_loc);

	auto addPow = awst::makeBigUIntBinOp(std::move(_left), awst::BigUIntBinaryOperator::Add, pow256, _loc);

	auto diff = awst::makeBigUIntBinOp(std::move(addPow), awst::BigUIntBinaryOperator::Sub, std::move(_right), _loc);

	return wrapMod256(std::move(diff), _loc);
}

std::shared_ptr<awst::Expression> buildSignedModDiv(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	BuilderBinaryOp _op,
	awst::SourceLocation const& _loc)
{
	// Two's complement: negative if value >= 2^255
	static constexpr char const* POW_2_255 =
		"57896044618658097711785492504343953926634992332820282019728792003956564819968";

	auto makeConst = [&](char const* val) {
		auto c = awst::makeIntegerConstant(val, _loc, awst::WType::biguintType());
		return c;
	};

	// isLeftNeg = left >= 2^255
	auto isLeftNeg = awst::makeNumericCompare(_left, awst::NumericComparison::Gte, makeConst(POW_2_255), _loc);

	// absLeft = isLeftNeg ? (2^256 - left) : left
	auto negLeft = awst::makeBigUIntBinOp(makeConst(kPow2_256), awst::BigUIntBinaryOperator::Sub, _left, _loc);

	auto absLeft = awst::makeConditional(
		isLeftNeg, std::move(negLeft), _left, awst::WType::biguintType(), _loc);

	// isRightNeg = right >= 2^255
	auto isRightNeg = awst::makeNumericCompare(_right, awst::NumericComparison::Gte, makeConst(POW_2_255), _loc);

	// absRight = isRightNeg ? (2^256 - right) : right
	auto negRight = awst::makeBigUIntBinOp(makeConst(kPow2_256), awst::BigUIntBinaryOperator::Sub, _right, _loc);

	auto absRight = awst::makeConditional(
		isRightNeg, std::move(negRight), _right, awst::WType::biguintType(), _loc);

	// Compute abs result
	awst::BigUIntBinaryOperator unsignedOp =
		(_op == BuilderBinaryOp::Mod)
		? awst::BigUIntBinaryOperator::Mod
		: awst::BigUIntBinaryOperator::FloorDiv;

	auto absResult = awst::makeBigUIntBinOp(std::move(absLeft), unsignedOp, std::move(absRight), _loc);

	// Apply sign:
	// mod: sign follows dividend (left)
	// div: sign is negative if signs differ
	auto negResult = awst::makeBigUIntBinOp(makeConst(kPow2_256), awst::BigUIntBinaryOperator::Sub, absResult, _loc);

	std::shared_ptr<awst::Expression> shouldNegate;
	if (_op == BuilderBinaryOp::Mod)
	{
		shouldNegate = isLeftNeg;
	}
	else
	{
		// div: negate if signs differ — XOR = eitherNeg && !bothNeg
		auto bothNeg = awst::makeBoolBinOp(isLeftNeg, awst::BinaryBooleanOperator::And, isRightNeg, _loc);

		auto eitherNeg = awst::makeBoolBinOp(isLeftNeg, awst::BinaryBooleanOperator::Or, isRightNeg, _loc);

		auto notBothNeg = awst::makeNot(std::move(bothNeg), _loc);

		auto xorSigns = awst::makeBoolBinOp(std::move(eitherNeg), awst::BinaryBooleanOperator::And, std::move(notBothNeg), _loc);
		shouldNegate = std::move(xorSigns);
	}

	// Only negate if result is non-zero (negating 0 gives 2^256)
	auto isZero = awst::makeNumericCompare(absResult, awst::NumericComparison::Eq, makeConst("0"), _loc);

	auto notZero = awst::makeNot(std::move(isZero), _loc);

	auto shouldNegateAndNonZero = awst::makeBoolBinOp(std::move(shouldNegate), awst::BinaryBooleanOperator::And, std::move(notZero), _loc);

	return awst::makeConditional(
		std::move(shouldNegateAndNonZero), std::move(negResult),
		std::move(absResult), awst::WType::biguintType(), _loc);
}

} // namespace puyasol::builder::eb
