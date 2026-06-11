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

std::shared_ptr<awst::Expression> buildBigUIntArithmeticShiftRight(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _shiftAmt,
	awst::SourceLocation const& _loc)
{
	auto* biguint = awst::WType::biguintType();
	auto* u64 = awst::WType::uint64Type();
	static int s_asrTemp = 0;
	int id = s_asrTemp++;
	std::string vName = "__asr_v_" + std::to_string(id);
	std::string nrName = "__asr_nr_" + std::to_string(id);
	std::string nName = "__asr_n_" + std::to_string(id);
	std::string sName = "__asr_s_" + std::to_string(id);
	auto vRead = [&]() { return awst::makeVarExpression(vName, biguint, _loc); };
	auto nrRead = [&]() { return awst::makeVarExpression(nrName, u64, _loc); };
	auto nRead = [&]() { return awst::makeVarExpression(nName, u64, _loc); };
	auto sRead = [&]() { return awst::makeVarExpression(sName, biguint, _loc); };

	// 2^n as a 32-byte biguint: setbit(bzero(32), 255-n, 1) (valid for n in [0,255]).
	auto pow2n = [&]() {
		auto bitIdx = awst::makeUInt64BinOp(
			awst::makeIntegerConstant("255", _loc), awst::UInt64BinaryOperator::Sub, nRead(), _loc);
		auto setbit = awst::makeSetbit(awst::makeBzero(32, _loc), std::move(bitIdx), awst::makeOne(_loc), _loc);
		return awst::makeAsBiguint(std::move(setbit), _loc);
	};

	// Pin value (read for the sign test + the floordiv) and the shift amount
	// (read for the clamp compare + branch) so each evaluates once.
	auto bindV = awst::makeAssignmentExpression(
		awst::makeVarExpression(vName, biguint, _loc), std::move(_value), _loc, biguint);
	auto bindNraw = awst::makeAssignmentExpression(
		awst::makeVarExpression(nrName, u64, _loc), std::move(_shiftAmt), _loc, u64);

	// nTmp = min(shift, 255): shifts >= 255 saturate (0 / all-ones), and the
	// setbit power trick is only valid for [0,255].
	auto nLt256 = awst::makeNumericCompare(
		nrRead(), awst::NumericComparison::Lt, awst::makeIntegerConstant("256", _loc), _loc);
	auto clampedN = awst::makeConditional(
		std::move(nLt256), nrRead(), awst::makeIntegerConstant("255", _loc), u64, _loc);
	auto bindN = awst::makeAssignmentExpression(
		awst::makeVarExpression(nName, u64, _loc), std::move(clampedN), _loc, u64);

	// sTmp = floordiv(v, 2^n) — the logical shift.
	auto shifted = awst::makeBigUIntBinOp(vRead(), awst::BigUIntBinaryOperator::FloorDiv, pow2n(), _loc);
	auto bindS = awst::makeAssignmentExpression(
		awst::makeVarExpression(sName, biguint, _loc), std::move(shifted), _loc, biguint);

	// fill = 2^256 - (2^256 / 2^n) = the top-n sign bits.
	auto fill = awst::makeBigUIntBinOp(
		makePow256(_loc), awst::BigUIntBinaryOperator::Sub,
		awst::makeBigUIntBinOp(makePow256(_loc), awst::BigUIntBinaryOperator::FloorDiv, pow2n(), _loc), _loc);

	// negative iff v >= 2^255.
	auto neg = awst::makeNumericCompare(
		vRead(), awst::NumericComparison::Gte,
		awst::makeIntegerConstant(
			"57896044618658097711785492504343953926634992332820282019728792003956564819968",
			_loc, biguint),
		_loc);

	// result = neg ? (sTmp | fill) : sTmp
	auto orFill = awst::makeBigUIntBinOp(sRead(), awst::BigUIntBinaryOperator::BitOr, std::move(fill), _loc);
	auto result = awst::makeConditional(std::move(neg), std::move(orFill), sRead(), biguint, _loc);

	auto comma = awst::makeCommaExpression(biguint, _loc);
	comma->expressions.push_back(std::move(bindV));
	comma->expressions.push_back(std::move(bindNraw));
	comma->expressions.push_back(std::move(bindN));
	comma->expressions.push_back(std::move(bindS));
	comma->expressions.push_back(std::move(result));
	return comma;
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
	// Both operands feed the underflow-assert AND the wrapped difference —
	// wrap so a side-effecting operand (`x -= f()`, the compound path that
	// bypasses SolBinaryOperation's wrap) evaluates once.
	_left = awst::makeEvalOnce(std::move(_left), _loc);
	_right = awst::makeEvalOnce(std::move(_right), _loc);

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
	// Each operand feeds the sign test, the negation, and the conditional's
	// else-branch (3 references each) — a side-effecting operand on the
	// compound path (`x %= f()`) ran once per reference. Pin both via the
	// same comma let-binding buildBigUIntArithmeticShiftRight uses, NOT
	// SingleEvaluation: for DIV the first _right reference sits inside the
	// short-circuit RHS of `Or(isLeftNeg, isRightNeg)`, so an SE temp would
	// be defined in a branch that doesn't dominate later uses (puya rejects
	// with "used but never defined"). The comma prologue evaluates both
	// bindings unconditionally before anything references them.
	auto* biguintW = awst::WType::biguintType();
	static int s_smdTemp = 0;
	int smdId = s_smdTemp++;
	std::string lName = "__smd_l_" + std::to_string(smdId);
	std::string rName = "__smd_r_" + std::to_string(smdId);
	auto bindL = awst::makeAssignmentExpression(
		awst::makeVarExpression(lName, biguintW, _loc), std::move(_left), _loc, biguintW);
	auto bindR = awst::makeAssignmentExpression(
		awst::makeVarExpression(rName, biguintW, _loc), std::move(_right), _loc, biguintW);
	_left = awst::makeVarExpression(lName, biguintW, _loc);
	_right = awst::makeVarExpression(rName, biguintW, _loc);

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

	// absResult is referenced three times below (negation, zero test, else
	// branch) — all PURE re-lowerings over the SE-pinned operands, so the
	// repeats are wasteful but correct. Do NOT makeEvalOnce-wrap it: its
	// first reference is `notZero` inside the short-circuit RHS of the final
	// condition's `And`, so the materialized temp would be defined in a
	// branch that doesn't dominate the conditional's else-arm — puya rejects
	// with "used but never defined: awst_tmp%N". SingleEvaluation is only
	// safe when the FIRST reference lowers unconditionally.
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

	auto finalCond = awst::makeConditional(
		std::move(shouldNegateAndNonZero), std::move(negResult),
		std::move(absResult), awst::WType::biguintType(), _loc);
	auto comma = awst::makeCommaExpression(awst::WType::biguintType(), _loc);
	comma->expressions.push_back(std::move(bindL));
	comma->expressions.push_back(std::move(bindR));
	comma->expressions.push_back(std::move(finalCond));
	return comma;
}

} // namespace puyasol::builder::eb
