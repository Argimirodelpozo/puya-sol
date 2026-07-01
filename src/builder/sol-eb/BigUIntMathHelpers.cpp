#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolutil/Numeric.h>

#include <sstream>
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

std::shared_ptr<awst::Expression> promoteToBiguint(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	if (_expr->wtype == awst::WType::biguintType())
		return _expr;

	// Integer constants: biguint constant directly (avoids itob(0) = 8 zero bytes).
	if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_expr.get()))
		return awst::makeIntegerConstant(intConst->value, _loc, awst::WType::biguintType());

	// account/bytes: reinterpret directly; itob is uint64-only and truncates >8 bytes.
	if (_expr->wtype == awst::WType::accountType()
		|| (_expr->wtype && _expr->wtype->kind() == awst::WTypeKind::Bytes))
	{
		auto bytesExpr = _expr->wtype == awst::WType::accountType()
			? awst::makeAsBytes(std::move(_expr), _loc)
			: std::move(_expr);
		return awst::makeAsBiguint(std::move(bytesExpr), _loc);
	}

	auto itob = awst::makeItob(std::move(_expr), _loc);
	return awst::makeAsBiguint(std::move(itob), _loc);
}

std::shared_ptr<awst::Expression> buildBigUIntShift(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _shiftAmt,
	bool _isLeftShift,
	awst::SourceLocation const& _loc)
{
	auto* biguint = awst::WType::biguintType();

	// EVM shl/shr saturate to 0 for shift >= 256 (EIP-145); Solidity's `<<`/`>>`
	// inherit it (shifts truncate, never overflow-check). The setbit-based 2^shift
	// indexes bit 255-shift, which underflows for shift >= 256 and panics on the AVM,
	// so clamp the index with `mod 256` AND select 0 for shift >= 256. Mirrors the
	// assembly handleShl/handleShr and the signed-SAR guard below.
	// SE: shift feeds both the index and the <256 guard (so `x << f()` runs f() once).
	auto shift = awst::makeSingleEvaluation(
		std::move(_shiftAmt), awst::WType::uint64Type(), awst::nextSingleEvalId(), _loc);

	// 2^(shift mod 256) via setbit(bzero(32), 255 - (shift mod 256), 1): index in [0,255].
	auto clampedShift = awst::makeUInt64BinOp(
		shift, awst::UInt64BinaryOperator::Mod, awst::makeIntegerConstant("256", _loc), _loc);
	auto bitIdx = awst::makeUInt64BinOp(
		awst::makeIntegerConstant("255", _loc), awst::UInt64BinaryOperator::Sub,
		std::move(clampedShift), _loc);
	auto castPow = awst::makeAsBiguint(
		awst::makeSetbit(awst::makeBzero(32, _loc), std::move(bitIdx), awst::makeOne(_loc), _loc),
		_loc);

	std::shared_ptr<awst::Expression> result = awst::makeBigUIntBinOp(std::move(_value),
		_isLeftShift ? awst::BigUIntBinaryOperator::Mult : awst::BigUIntBinaryOperator::FloorDiv,
		std::move(castPow), _loc);

	// Left shift must wrap mod 2^256 (EVM semantics).
	if (_isLeftShift)
		result = wrapMod256(std::move(result), _loc);

	// shift >= 256 → 0.
	auto lt256 = awst::makeNumericCompare(shift, awst::NumericComparison::Lt,
		awst::makeIntegerConstant("256", _loc, awst::WType::uint64Type()), _loc);
	return awst::makeConditional(std::move(lt256), std::move(result),
		awst::makeIntegerConstant("0", _loc, biguint), biguint, _loc);
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

	// Pin both operands (each referenced 2-3x) so side effects run once.
	auto bindV = awst::makeAssignmentExpression(
		awst::makeVarExpression(vName, biguint, _loc), std::move(_value), _loc, biguint);
	auto bindNraw = awst::makeAssignmentExpression(
		awst::makeVarExpression(nrName, u64, _loc), std::move(_shiftAmt), _loc, u64);

	// Clamp shift to 255: setbit trick invalid above 255, and >=255 saturates.
	auto nLt256 = awst::makeNumericCompare(
		nrRead(), awst::NumericComparison::Lt, awst::makeIntegerConstant("256", _loc), _loc);
	auto clampedN = awst::makeConditional(
		std::move(nLt256), nrRead(), awst::makeIntegerConstant("255", _loc), u64, _loc);
	auto bindN = awst::makeAssignmentExpression(
		awst::makeVarExpression(nName, u64, _loc), std::move(clampedN), _loc, u64);

	// Logical shift (unsigned floordiv).
	auto shifted = awst::makeBigUIntBinOp(vRead(), awst::BigUIntBinaryOperator::FloorDiv, pow2n(), _loc);
	auto bindS = awst::makeAssignmentExpression(
		awst::makeVarExpression(sName, biguint, _loc), std::move(shifted), _loc, biguint);

	// fill = top-n sign bits.
	auto fill = awst::makeBigUIntBinOp(
		makePow256(_loc), awst::BigUIntBinaryOperator::Sub,
		awst::makeBigUIntBinOp(makePow256(_loc), awst::BigUIntBinaryOperator::FloorDiv, pow2n(), _loc), _loc);

	// negative iff v >= 2^255.
	auto neg = TypeCoercion::isNegativeSigned(vRead(), 256, _loc);

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

	// Unchecked: wrap products mod 2^256 (huge exponents e.g. 2**1113 overflow biguint).
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
	// Both operands referenced twice (assert + diff); wrap to avoid double-eval
	// on compound path (`x -= f()` bypasses SolBinaryOperation's eval-once).
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
	unsigned _resultBits,
	bool _checked,
	awst::SourceLocation const& _loc)
{
	bool const isDiv = (_op != BuilderBinaryOp::Mod);

	// Each operand is referenced 3x (sign test, negation, else-branch) — and again
	// by the overflow guard below. Pin via comma let-binding (not SingleEvaluation):
	// for DIV the first _right ref sits inside the short-circuit RHS of
	// Or(isLeftNeg, isRightNeg) — an SE temp defined there doesn't dominate later
	// uses (puya: "used but never defined").
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

	auto makeConst = [&](char const* val) {
		auto c = awst::makeIntegerConstant(val, _loc, awst::WType::biguintType());
		return c;
	};

	// Signed-division overflow: intN.min / -1 = +2^(N-1) doesn't fit intN → EVM reverts,
	// but the abs-value arithmetic below just wraps it back to intN.min. Guard it (div +
	// checked only; mod never overflows). Operands are canonical 256-bit two's complement,
	// so intN.min reads as 2^256 - 2^(resultBits-1) and -1 as 2^256-1 (both computed via
	// u256's mod-2^256 wrap). Pushed into the comma before the divide so it runs first.
	std::shared_ptr<awst::Expression> overflowAssert;
	if (isDiv && _checked)
	{
		std::string minStr = (solidity::u256(0) - (solidity::u256(1) << (_resultBits - 1))).str();
		std::string negOneStr = (solidity::u256(0) - 1).str();
		auto xIsMin = awst::makeNumericCompare(
			awst::makeVarExpression(lName, biguintW, _loc), awst::NumericComparison::Eq,
			awst::makeIntegerConstant(minStr, _loc, biguintW), _loc);
		auto yIsNeg1 = awst::makeNumericCompare(
			awst::makeVarExpression(rName, biguintW, _loc), awst::NumericComparison::Eq,
			awst::makeIntegerConstant(negOneStr, _loc, biguintW), _loc);
		auto bothTrue = awst::makeBoolBinOp(
			std::move(xIsMin), awst::BinaryBooleanOperator::And, std::move(yIsNeg1), _loc);
		overflowAssert = awst::makeAssert(
			awst::makeNot(std::move(bothTrue), _loc), _loc, "signed division overflow");
	}

	// isLeftNeg = left >= 2^255
	auto isLeftNeg = TypeCoercion::isNegativeSigned(_left, 256, _loc);

	// absLeft = isLeftNeg ? (2^256 - left) : left
	auto negLeft = awst::makeBigUIntBinOp(makeConst(kPow2_256), awst::BigUIntBinaryOperator::Sub, _left, _loc);

	auto absLeft = awst::makeConditional(
		isLeftNeg, std::move(negLeft), _left, awst::WType::biguintType(), _loc);

	// isRightNeg = right >= 2^255
	auto isRightNeg = TypeCoercion::isNegativeSigned(_right, 256, _loc);

	// absRight = isRightNeg ? (2^256 - right) : right
	auto negRight = awst::makeBigUIntBinOp(makeConst(kPow2_256), awst::BigUIntBinaryOperator::Sub, _right, _loc);

	auto absRight = awst::makeConditional(
		isRightNeg, std::move(negRight), _right, awst::WType::biguintType(), _loc);

	// Compute abs result
	awst::BigUIntBinaryOperator unsignedOp =
		(_op == BuilderBinaryOp::Mod)
		? awst::BigUIntBinaryOperator::Mod
		: awst::BigUIntBinaryOperator::FloorDiv;

	// absResult is referenced 3x; repeats are wasteful but correct. Cannot
	// makeEvalOnce: its first ref is `notZero` inside the And's short-circuit RHS
	// — a SE temp there doesn't dominate the else-arm (puya: "used but never defined").
	auto absResult = awst::makeBigUIntBinOp(std::move(absLeft), unsignedOp, std::move(absRight), _loc);

	// Sign: mod follows dividend; div negates if signs differ.
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

	std::shared_ptr<awst::Expression> result = awst::makeConditional(
		std::move(shouldNegateAndNonZero), std::move(negResult),
		std::move(absResult), awst::WType::biguintType(), _loc);

	// Narrow the 256-bit two's-complement result to the result type's native width:
	// <=64-bit → uint64 (low 64-bit TC, so a sub-word result composes with surrounding
	// uint64 ops); >64-bit stays canonical 256-bit biguint (already sign-extended).
	if (_resultBits <= 64)
		result = awst::makeBiguintToUInt64(std::move(result), _loc);

	auto* rwtype = result->wtype;
	auto comma = awst::makeCommaExpression(rwtype, _loc);
	comma->expressions.push_back(std::move(bindL));
	comma->expressions.push_back(std::move(bindR));
	if (overflowAssert)
		comma->expressions.push_back(std::move(overflowAssert));
	comma->expressions.push_back(std::move(result));
	return comma;
}

std::shared_ptr<awst::Expression> buildSignedArithmetic(
	ContractContext& _ctx,
	bool _isUnchecked,
	BuilderBinaryOp _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	unsigned _bits,
	awst::SourceLocation const& _loc)
{
	bool isBiguint = (_bits > 64);
	bool isSub = (_op == BuilderBinaryOp::Sub);
	bool isMul = (_op == BuilderBinaryOp::Mult);

	// 2^N (wrap modulus) and 2^(N-1) (sign-bit boundary).
	auto [pow2NStr, halfNStr] = TypeCoercion::pow2NAndHalf(_bits);

	auto makeBiguintConst = [&](std::string const& val) {
		return awst::makeIntegerConstant(val, _loc, awst::WType::biguintType());
	};

	// Pin operands: the checked overflow assert below references each again, so a
	// side-effecting operand (`x += f()`) must evaluate exactly once. Self-contained
	// here (idempotent — the plain `a+b` caller already passes SE-wrapped operands;
	// the compound path via SolIntegerBuilder::binary_op previously relied on it).
	_left = awst::makeEvalOnce(std::move(_left), _loc);
	_right = awst::makeEvalOnce(std::move(_right), _loc);

	_left = promoteToBiguint(std::move(_left), _loc);
	_right = promoteToBiguint(std::move(_right), _loc);

	// Mask to N bits (uint64 two's-comp may exceed 2^N, e.g. int8(-2)=2^64-2).
	if (_bits < 256)
	{
		auto maskOp = [&](std::shared_ptr<awst::Expression> v) {
			return awst::makeBigUIntBinOp(std::move(v), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), _loc);
		};
		_left = maskOp(std::move(_left));
		_right = maskOp(std::move(_right));
	}

	// Step 1: unsigned op in biguint. Sub uses (a + 2^N - b) to stay non-negative.
	std::shared_ptr<awst::Expression> rawResult;
	if (isSub)
	{
		auto aPlusPow = awst::makeBigUIntBinOp(_left, awst::BigUIntBinaryOperator::Add, makeBiguintConst(pow2NStr), _loc);
		rawResult = awst::makeBigUIntBinOp(std::move(aPlusPow), awst::BigUIntBinaryOperator::Sub, _right, _loc);
	}
	else
	{
		auto bigOp = isMul ? awst::BigUIntBinaryOperator::Mult : awst::BigUIntBinaryOperator::Add;
		rawResult = awst::makeBigUIntBinOp(_left, bigOp, _right, _loc);
	}

	// Step 2: wrap mod 2^N (two's complement).
	rawResult = awst::makeBigUIntBinOp(std::move(rawResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), _loc);

	// Step 3: signed overflow check (skipped when unchecked).
	if (!_isUnchecked)
	{
		auto isNeg = [&](std::shared_ptr<awst::Expression> const& val) {
			auto cmp = awst::makeNumericCompare(promoteToBiguint(val, _loc), awst::NumericComparison::Lt, makeBiguintConst(halfNStr), _loc);
			return awst::makeNot(std::move(cmp), _loc);
		};
		std::shared_ptr<awst::Expression> overflowCond;
		if (!isSub && !isMul) // add: no overflow iff a_neg != b_neg || a_neg == result_neg
		{
			auto aNeg = isNeg(_left), bNeg = isNeg(_right), rNeg = isNeg(rawResult);
			overflowCond = awst::makeBoolBinOp(
				awst::makeNumericCompare(aNeg, awst::NumericComparison::Ne, bNeg, _loc),
				awst::BinaryBooleanOperator::Or,
				awst::makeNumericCompare(aNeg, awst::NumericComparison::Eq, rNeg, _loc), _loc);
		}
		else if (isSub) // sub: no overflow iff a_neg == b_neg || a_neg == result_neg
		{
			auto aNeg = isNeg(_left), bNeg = isNeg(_right), rNeg = isNeg(rawResult);
			overflowCond = awst::makeBoolBinOp(
				awst::makeNumericCompare(aNeg, awst::NumericComparison::Eq, bNeg, _loc),
				awst::BinaryBooleanOperator::Or,
				awst::makeNumericCompare(aNeg, awst::NumericComparison::Eq, rNeg, _loc), _loc);
		}
		else // mul: abs(a)*abs(b) in range, or b==0
		{
			auto absVal = [&](std::shared_ptr<awst::Expression> const& val) {
				auto neg = isNeg(val);
				auto negated = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, val, _loc);
				return awst::makeConditional(std::move(neg), std::move(negated), val, awst::WType::biguintType(), _loc);
			};
			auto absProduct = awst::makeBigUIntBinOp(absVal(_left), awst::BigUIntBinaryOperator::Mult, absVal(_right), _loc);
			auto sameSign = awst::makeNumericCompare(isNeg(_left), awst::NumericComparison::Eq, isNeg(_right), _loc);
			auto ltHalf = awst::makeNumericCompare(absProduct, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), _loc);
			auto leHalf = awst::makeNot(awst::makeNumericCompare(makeBiguintConst(halfNStr), awst::NumericComparison::Lt, absProduct, _loc), _loc);
			auto rangeCheck = awst::makeConditional(std::move(sameSign), std::move(ltHalf), std::move(leHalf), awst::WType::boolType(), _loc);
			auto bZero = awst::makeNumericCompare(_right, awst::NumericComparison::Eq, makeBiguintConst("0"), _loc);
			overflowCond = awst::makeBoolBinOp(std::move(bZero), awst::BinaryBooleanOperator::Or, std::move(rangeCheck), _loc);
		}
		if (overflowCond)
			_ctx.prePendingStatements.push_back(awst::makeExpressionStatement(
				awst::makeAssert(std::move(overflowCond), _loc, "signed arithmetic overflow"), _loc));
	}

	// Step 4: ≤64-bit → uint64; sub-256 biguint → canonical 256-bit two's complement.
	if (!isBiguint)
		return awst::makeBiguintToUInt64(std::move(rawResult), _loc);
	if (_bits < 256)
		return TypeCoercion::signExtendToUint256(std::move(rawResult), _bits, _loc);
	return rawResult;
}

} // namespace puyasol::builder::eb
