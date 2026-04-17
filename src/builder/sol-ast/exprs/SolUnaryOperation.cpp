/// @file SolUnaryOperation.cpp
/// Migrated from UnaryOperationBuilder.cpp.

#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/exprs/SolUnaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>
#include <sstream>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolUnaryOperation::SolUnaryOperation(
	eb::BuilderContext& _ctx, UnaryOperation const& _node)
	: SolExpression(_ctx, _node), m_unaryOp(_node)
{
}

bool SolUnaryOperation::isBigUInt(awst::WType const* _type) const
{
	return _type == awst::WType::biguintType();
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleNot(
	std::shared_ptr<awst::Expression> _operand)
{
	auto e = std::make_shared<awst::Not>();
	e->sourceLocation = m_loc;
	e->wtype = awst::WType::boolType();
	e->expr = std::move(_operand);
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleNegate(
	std::shared_ptr<awst::Expression> _operand)
{
	// Check if the operand or result type is a signed integer.
	// For constant expressions like (-2), the operand type is RationalNumberType
	// but the result type is signed IntegerType.
	auto const* solType = m_unaryOp.subExpression().annotation().type;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solType))
		solType = &udvt->underlyingType();
	auto const* intType = dynamic_cast<IntegerType const*>(solType);
	if (!intType || !intType->isSigned())
	{
		// Try the result type (for constant expressions)
		auto const* resultType = m_unaryOp.annotation().type;
		if (auto const* udvt2 = dynamic_cast<UserDefinedValueType const*>(resultType))
			resultType = &udvt2->underlyingType();
		auto const* resultIntType = dynamic_cast<IntegerType const*>(resultType);
		if (resultIntType && resultIntType->isSigned())
			intType = resultIntType;
	}

	if (intType && intType->isSigned())
	{
		unsigned bits = intType->numBits();
		// Signed negation: -x = (2^N - x) mod 2^N
		// Overflow check: -INT_MIN overflows (x == 2^(N-1))
		std::string pow2NStr, halfNStr;
		if (bits == 256)
		{
			pow2NStr = kPow2_256;
			halfNStr = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
		}
		else
		{
			solidity::u256 pow2N = solidity::u256(1) << bits;
			solidity::u256 halfN = solidity::u256(1) << (bits - 1);
			std::ostringstream oss1, oss2;
			oss1 << pow2N; pow2NStr = oss1.str();
			oss2 << halfN; halfNStr = oss2.str();
		}

		auto makeBiguintConst = [&](std::string const& val) {
			auto c = awst::makeIntegerConstant(val, m_loc, awst::WType::biguintType());
			return c;
		};

		// Ensure operand is biguint
		auto operand = std::move(_operand);
		if (operand->wtype == awst::WType::uint64Type())
		{
			auto itob = std::make_shared<awst::IntrinsicCall>();
			itob->sourceLocation = m_loc;
			itob->wtype = awst::WType::bytesType();
			itob->opCode = "itob";
			itob->stackArgs.push_back(std::move(operand));
			auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
			operand = std::move(cast);
		}

		// Mask to N bits first (uint64 two's complement may be wider)
		if (bits < 256)
		{
			auto mask = std::make_shared<awst::BigUIntBinaryOperation>();
			mask->sourceLocation = m_loc;
			mask->wtype = awst::WType::biguintType();
			mask->left = std::move(operand);
			mask->op = awst::BigUIntBinaryOperator::Mod;
			mask->right = makeBiguintConst(pow2NStr);
			operand = std::move(mask);
		}

		// Checked: assert(x != INT_MIN) i.e. x != 2^(N-1)
		if (!m_ctx.inUncheckedBlock)
		{
			auto cmp = std::make_shared<awst::NumericComparisonExpression>();
			cmp->sourceLocation = m_loc;
			cmp->wtype = awst::WType::boolType();
			cmp->lhs = operand;
			cmp->op = awst::NumericComparison::Ne;
			cmp->rhs = makeBiguintConst(halfNStr);

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), m_loc, "signed negation overflow"), m_loc);
			m_ctx.prePendingStatements.push_back(std::move(assertStmt));
		}

		// -x = (2^N - x) mod 2^N
		// For 256-bit: 2^256 - 0 would overflow, so use if/else via temp variable
		std::shared_ptr<awst::Expression> negated;
		if (bits == 256)
		{
			// Emit: __neg_tmp = 0; if (x != 0) { __neg_tmp = (2^256 - x) % 2^256; }
			// Then use __neg_tmp as the result.
			std::string tmpName = "__neg_tmp_" + std::to_string(m_unaryOp.id());

			// __neg_tmp = 0
			auto tmpVar = awst::makeVarExpression(tmpName, awst::WType::biguintType(), m_loc);

			auto initStmt = std::make_shared<awst::AssignmentStatement>();
			initStmt->sourceLocation = m_loc;
			initStmt->target = tmpVar;
			initStmt->value = makeBiguintConst("0");
			m_ctx.prePendingStatements.push_back(std::move(initStmt));

			// if (x != 0) { __neg_tmp = (2^256 - x) % 2^256; }
			auto isNonZero = std::make_shared<awst::NumericComparisonExpression>();
			isNonZero->sourceLocation = m_loc;
			isNonZero->wtype = awst::WType::boolType();
			isNonZero->lhs = operand;
			isNonZero->op = awst::NumericComparison::Ne;
			isNonZero->rhs = makeBiguintConst("0");

			auto sub = std::make_shared<awst::BigUIntBinaryOperation>();
			sub->sourceLocation = m_loc;
			sub->wtype = awst::WType::biguintType();
			sub->left = makeBiguintConst(pow2NStr);
			sub->op = awst::BigUIntBinaryOperator::Sub;
			sub->right = operand;

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = m_loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(sub);
			mod->op = awst::BigUIntBinaryOperator::Mod;
			mod->right = makeBiguintConst(pow2NStr);

			auto assignTmp = std::make_shared<awst::AssignmentStatement>();
			assignTmp->sourceLocation = m_loc;
			assignTmp->target = tmpVar;
			assignTmp->value = std::move(mod);

			auto ifBody = std::make_shared<awst::Block>();
			ifBody->sourceLocation = m_loc;
			ifBody->body.push_back(std::move(assignTmp));

			auto ifElse = std::make_shared<awst::IfElse>();
			ifElse->sourceLocation = m_loc;
			ifElse->condition = std::move(isNonZero);
			ifElse->ifBranch = std::move(ifBody);
			m_ctx.prePendingStatements.push_back(std::move(ifElse));

			negated = tmpVar;
		}
		else
		{
			auto sub = std::make_shared<awst::BigUIntBinaryOperation>();
			sub->sourceLocation = m_loc;
			sub->wtype = awst::WType::biguintType();
			sub->left = makeBiguintConst(pow2NStr);
			sub->op = awst::BigUIntBinaryOperator::Sub;
			sub->right = std::move(operand);

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = m_loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(sub);
			mod->op = awst::BigUIntBinaryOperator::Mod;
			mod->right = makeBiguintConst(pow2NStr);

			negated = std::move(mod);
		}

		// Convert back to uint64 for ≤64-bit types
		if (bits <= 64)
		{
			auto castBytes = awst::makeReinterpretCast(std::move(negated), awst::WType::bytesType(), m_loc);

			auto eight = awst::makeIntegerConstant("8", m_loc);
			auto bz = std::make_shared<awst::IntrinsicCall>();
			bz->sourceLocation = m_loc;
			bz->wtype = awst::WType::bytesType();
			bz->opCode = "bzero";
			bz->stackArgs.push_back(eight);
			auto cat = std::make_shared<awst::IntrinsicCall>();
			cat->sourceLocation = m_loc;
			cat->wtype = awst::WType::bytesType();
			cat->opCode = "concat";
			cat->stackArgs.push_back(std::move(bz));
			cat->stackArgs.push_back(std::move(castBytes));
			auto lenCall = std::make_shared<awst::IntrinsicCall>();
			lenCall->sourceLocation = m_loc;
			lenCall->wtype = awst::WType::uint64Type();
			lenCall->opCode = "len";
			lenCall->stackArgs.push_back(cat);
			auto eight2 = awst::makeIntegerConstant("8", m_loc);
			auto start = std::make_shared<awst::IntrinsicCall>();
			start->sourceLocation = m_loc;
			start->wtype = awst::WType::uint64Type();
			start->opCode = "-";
			start->stackArgs.push_back(std::move(lenCall));
			start->stackArgs.push_back(eight2);
			auto extract = std::make_shared<awst::IntrinsicCall>();
			extract->sourceLocation = m_loc;
			extract->wtype = awst::WType::uint64Type();
			extract->opCode = "extract_uint64";
			extract->stackArgs.push_back(cat);
			extract->stackArgs.push_back(std::move(start));
			return extract;
		}
		return negated;
	}

	// For biguint constant negation (e.g., (-2) where type is RationalNumberType),
	// produce two's complement directly: 2^256 - value
	if (isBigUInt(_operand->wtype))
	{
		if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_operand.get()))
		{
			solidity::u256 val(intConst->value);
			// Allow val == 2^255 (the largest positive value whose negation
			// is a valid signed int256: -2^255 = type(int256).min). The strict
			// less-than missed this, so `-2**255` fell through to runtime
			// `0 - 2^255`, which crashes with "byte math would have negative
			// result" since AVM `b-` rejects negative results.
			if (val > 0 && val <= (solidity::u256(1) << 255))
			{
				// 2^256 - val (two's complement negation)
				static const std::string pow256Str =
					kPow2_256;
				solidity::u256 pow256(pow256Str);
				solidity::u256 negVal = pow256 - val;
				std::ostringstream oss;
				oss << negVal;
				auto result = awst::makeIntegerConstant(oss.str(), m_loc, awst::WType::biguintType());
				return result;
			}
		}

		// Non-constant unsigned negation: 0 - x
		auto zero = awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType());
		auto e = std::make_shared<awst::BigUIntBinaryOperation>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::biguintType();
		e->left = std::move(zero);
		e->op = awst::BigUIntBinaryOperator::Sub;
		e->right = std::move(_operand);
		return e;
	}
	// For uint64 constant negation, produce two's complement biguint.
	// We can't safely narrow to uint64 here — the surrounding context
	// might be int256 (biguint storage) and a uint64 result would
	// sign-extend wrong (uint64(2^64-1) → biguint(2^64-1) instead of
	// biguint(2^256-1)). The wide-form constant survives narrowing
	// to uint64 slots correctly via `extract3 + btoi`, while uint64
	// constants don't survive widening to biguint slots.
	if (_operand->wtype == awst::WType::uint64Type())
	{
		if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_operand.get()))
		{
			solidity::u256 val(intConst->value);
			if (val > 0)
			{
				static const std::string pow256Str =
					kPow2_256;
				solidity::u256 pow256(pow256Str);
				solidity::u256 negVal = pow256 - val;
				std::ostringstream oss;
				oss << negVal;
				auto result = awst::makeIntegerConstant(oss.str(), m_loc, awst::WType::biguintType());
				return result;
			}
		}
	}
	auto zero2 = awst::makeIntegerConstant("0", m_loc, _operand->wtype);
	if (_operand->wtype == awst::WType::uint64Type())
	{
		auto e = std::make_shared<awst::UInt64BinaryOperation>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::uint64Type();
		e->left = std::move(zero2);
		e->op = awst::UInt64BinaryOperator::Sub;
		e->right = std::move(_operand);
		return e;
	}
	auto e = std::make_shared<awst::BigUIntBinaryOperation>();
	e->sourceLocation = m_loc;
	e->wtype = awst::WType::biguintType();
	e->left = std::move(zero2);
	e->op = awst::BigUIntBinaryOperator::Sub;
	e->right = std::move(_operand);
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleBitNot(
	std::shared_ptr<awst::Expression> _operand)
{
	auto* resultType = _operand->wtype;
	if (_operand->wtype == awst::WType::uint64Type())
	{
		// Use the correct bit-width mask from the Solidity type
		unsigned maskBits = 64;
		auto const* solType = m_unaryOp.subExpression().annotation().type;
		if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solType))
			solType = &udvt->underlyingType();
		if (auto const* intType = dynamic_cast<IntegerType const*>(solType))
			maskBits = intType->numBits();

		solidity::u256 mask = (maskBits >= 64)
			? solidity::u256("18446744073709551615")
			: (solidity::u256(1) << maskBits) - 1;
		std::ostringstream oss;
		oss << mask;

		auto maxVal = awst::makeIntegerConstant(oss.str(), m_loc);
		auto xorOp = std::make_shared<awst::UInt64BinaryOperation>();
		xorOp->sourceLocation = m_loc;
		xorOp->wtype = awst::WType::uint64Type();
		xorOp->left = std::move(_operand);
		xorOp->right = std::move(maxVal);
		xorOp->op = awst::UInt64BinaryOperator::BitXor;
		return xorOp;
	}
	auto expr = std::move(_operand);
	if (expr->wtype == awst::WType::biguintType())
	{
		auto cast = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), m_loc);
		expr = std::move(cast);
	}
	auto e = std::make_shared<awst::BytesUnaryOperation>();
	e->sourceLocation = m_loc;
	e->wtype = expr->wtype;
	e->op = awst::BytesUnaryOperator::BitInvert;
	e->expr = std::move(expr);
	if (resultType == awst::WType::biguintType())
	{
		auto castBack = awst::makeReinterpretCast(std::move(e), resultType, m_loc);
		return castBack;
	}
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleIncDec(
	std::shared_ptr<awst::Expression> _operand)
{
	bool isPrefix = m_unaryOp.isPrefixOperation();
	bool isInc = (m_unaryOp.getOperator() == Token::Inc);

	bool isSigned = false;
	if (auto const* intType = dynamic_cast<IntegerType const*>(
			m_unaryOp.subExpression().annotation().type))
		isSigned = intType->isSigned();

	// Unwrap BoxValueExpression for reads
	if (dynamic_cast<awst::BoxValueExpression const*>(_operand.get()))
	{
		auto defaultVal = builder::StorageMapper::makeDefaultValue(_operand->wtype, m_loc);
		auto stateGet = std::make_shared<awst::StateGet>();
		stateGet->sourceLocation = m_loc;
		stateGet->wtype = _operand->wtype;
		stateGet->field = _operand;
		stateGet->defaultValue = defaultVal;
		_operand = std::move(stateGet);
	}

	static const std::string pow256 =
		kPow2_256;
	static const std::string pow256Minus1 =
		"115792089237316195423570985008687907853269984665640564039457584007913129639935";

	// Get signed bit width for overflow checks
	unsigned signedBits = 0;
	if (isSigned)
	{
		if (auto const* it = dynamic_cast<IntegerType const*>(
				m_unaryOp.subExpression().annotation().type))
			signedBits = it->numBits();
	}

	auto makeNewValue = [&](std::shared_ptr<awst::Expression> base)
		-> std::shared_ptr<awst::Expression>
	{
		if (isSigned && signedBits > 0)
		{
			// Signed inc/dec with two's complement wrapping + overflow check
			std::string pow2NStr2, halfNStr2;
			if (signedBits == 256)
			{
				pow2NStr2 = pow256;
				halfNStr2 = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
			}
			else
			{
				solidity::u256 p = solidity::u256(1) << signedBits;
				solidity::u256 h = solidity::u256(1) << (signedBits - 1);
				std::ostringstream o1, o2;
				o1 << p; pow2NStr2 = o1.str();
				o2 << h; halfNStr2 = o2.str();
			}

			auto makeBConst = [&](std::string const& v) {
				auto c = awst::makeIntegerConstant(v, m_loc, awst::WType::biguintType());
				return c;
			};

			// Ensure biguint
			auto val = std::move(base);
			if (val->wtype == awst::WType::uint64Type())
			{
				auto itob = std::make_shared<awst::IntrinsicCall>();
				itob->sourceLocation = m_loc;
				itob->wtype = awst::WType::bytesType();
				itob->opCode = "itob";
				itob->stackArgs.push_back(std::move(val));
				auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
				val = std::move(cast);
			}

			// Mask to N bits
			if (signedBits < 256)
			{
				auto mask = std::make_shared<awst::BigUIntBinaryOperation>();
				mask->sourceLocation = m_loc;
				mask->wtype = awst::WType::biguintType();
				mask->left = std::move(val);
				mask->op = awst::BigUIntBinaryOperator::Mod;
				mask->right = makeBConst(pow2NStr2);
				val = std::move(mask);
			}

			// Checked overflow: inc overflows at MAX (half-1), dec underflows at MIN (half)
			if (!m_ctx.inUncheckedBlock)
			{
				std::string limitStr;
				if (isInc)
				{
					// MAX = half - 1
					solidity::u256 maxVal = (solidity::u256(1) << (signedBits - 1)) - 1;
					std::ostringstream oss; oss << maxVal; limitStr = oss.str();
				}
				else
					limitStr = halfNStr2; // MIN = half

				auto cmp = std::make_shared<awst::NumericComparisonExpression>();
				cmp->sourceLocation = m_loc;
				cmp->wtype = awst::WType::boolType();
				cmp->lhs = val;
				cmp->op = awst::NumericComparison::Ne;
				cmp->rhs = makeBConst(limitStr);

				auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), m_loc, "signed inc/dec overflow"), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(assertStmt));
			}

			// Compute: inc → (val + 1) mod 2^N, dec → (val + 2^N - 1) mod 2^N
			std::shared_ptr<awst::Expression> added;
			if (isInc)
			{
				auto add = std::make_shared<awst::BigUIntBinaryOperation>();
				add->sourceLocation = m_loc;
				add->wtype = awst::WType::biguintType();
				add->left = std::move(val);
				add->op = awst::BigUIntBinaryOperator::Add;
				add->right = makeBConst("1");
				added = std::move(add);
			}
			else
			{
				// val + (2^N - 1) to avoid negative
				solidity::u256 decOffset = (signedBits == 256)
					? solidity::u256(0) // special: use string directly
					: (solidity::u256(1) << signedBits) - 1;
				std::string decOffsetStr;
				if (signedBits == 256)
					decOffsetStr = pow256Minus1;
				else
				{
					std::ostringstream oss; oss << decOffset; decOffsetStr = oss.str();
				}
				auto add = std::make_shared<awst::BigUIntBinaryOperation>();
				add->sourceLocation = m_loc;
				add->wtype = awst::WType::biguintType();
				add->left = std::move(val);
				add->op = awst::BigUIntBinaryOperator::Add;
				add->right = makeBConst(decOffsetStr);
				added = std::move(add);
			}

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = m_loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(added);
			mod->op = awst::BigUIntBinaryOperator::Mod;
			mod->right = makeBConst(pow2NStr2);

			// Convert back to uint64 for ≤64-bit types
			if (signedBits <= 64)
			{
				auto castBytes = awst::makeReinterpretCast(std::move(mod), awst::WType::bytesType(), m_loc);
				auto eight = awst::makeIntegerConstant("8", m_loc);
				auto bz = std::make_shared<awst::IntrinsicCall>();
				bz->sourceLocation = m_loc;
				bz->wtype = awst::WType::bytesType();
				bz->opCode = "bzero";
				bz->stackArgs.push_back(eight);
				auto cat = std::make_shared<awst::IntrinsicCall>();
				cat->sourceLocation = m_loc;
				cat->wtype = awst::WType::bytesType();
				cat->opCode = "concat";
				cat->stackArgs.push_back(std::move(bz));
				cat->stackArgs.push_back(std::move(castBytes));
				auto lenCall = std::make_shared<awst::IntrinsicCall>();
				lenCall->sourceLocation = m_loc;
				lenCall->wtype = awst::WType::uint64Type();
				lenCall->opCode = "len";
				lenCall->stackArgs.push_back(cat);
				auto eight2 = awst::makeIntegerConstant("8", m_loc);
				auto start = std::make_shared<awst::IntrinsicCall>();
				start->sourceLocation = m_loc;
				start->wtype = awst::WType::uint64Type();
				start->opCode = "-";
				start->stackArgs.push_back(std::move(lenCall));
				start->stackArgs.push_back(eight2);
				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = m_loc;
				extract->wtype = awst::WType::uint64Type();
				extract->opCode = "extract_uint64";
				extract->stackArgs.push_back(cat);
				extract->stackArgs.push_back(std::move(start));
				return extract;
			}
			return mod;
		}
		else if (isBigUInt(base->wtype))
		{
			auto one = awst::makeIntegerConstant("1", m_loc, awst::WType::biguintType());
			auto bin = std::make_shared<awst::BigUIntBinaryOperation>();
			bin->sourceLocation = m_loc;
			bin->wtype = awst::WType::biguintType();
			bin->left = std::move(base);
			bin->op = isInc ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub;
			bin->right = std::move(one);
			return bin;
		}
		else
		{
			auto one = awst::makeIntegerConstant("1", m_loc);
			auto bin = std::make_shared<awst::UInt64BinaryOperation>();
			bin->sourceLocation = m_loc;
			bin->wtype = awst::WType::uint64Type();
			bin->left = std::move(base);
			bin->op = isInc ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub;
			bin->right = std::move(one);
			return bin;
		}
	};

	// Re-read the subexpression for the assignment target. State vars come
	// back wrapped in StateGet, which is a read — unwrap so the assignment
	// lands on the writable BoxValueExpression / AppStateExpression.
	auto makeWriteTarget = [&]() -> std::shared_ptr<awst::Expression>
	{
		auto target = buildExpr(m_unaryOp.subExpression());
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
			target = sg->field;
		return target;
	};

	if (isPrefix)
	{
		auto newValue = makeNewValue(_operand);
		auto assignExpr = std::make_shared<awst::AssignmentExpression>();
		assignExpr->sourceLocation = m_loc;
		assignExpr->wtype = _operand->wtype;
		assignExpr->target = makeWriteTarget();
		assignExpr->value = std::move(newValue);
		return assignExpr;
	}
	else
	{
		// Post-increment/decrement: capture the old value in a temp var,
		// emit the variable update as a *pre-pending* statement so it
		// happens before any sibling reads of the same variable, then
		// return the saved old value.
		//
		// Without this, `return a++ + a` evaluates `a` (2nd operand)
		// against the *old* value of `a` because the post-inc assignment
		// sat in pendingStatements — which only fires at the end of the
		// enclosing statement.
		static int postIncCounter = 0;
		std::string tempName = "__postinc_" + std::to_string(postIncCounter++);

		auto tempVar = awst::makeVarExpression(tempName, _operand->wtype, m_loc);

		// Save old value: temp = a
		auto saveStmt = std::make_shared<awst::AssignmentStatement>();
		saveStmt->sourceLocation = m_loc;
		saveStmt->target = tempVar;
		saveStmt->value = _operand;
		m_ctx.prePendingStatements.push_back(std::move(saveStmt));

		// Compute new value from the saved temp (not re-reading a)
		auto newValue = makeNewValue(tempVar);

		// a = temp + 1 (for inc) or temp - 1 (for dec)
		auto incrStmt = std::make_shared<awst::AssignmentStatement>();
		incrStmt->sourceLocation = m_loc;
		incrStmt->target = makeWriteTarget();
		incrStmt->value = std::move(newValue);
		m_ctx.prePendingStatements.push_back(std::move(incrStmt));

		return tempVar;
	}
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleDelete(
	std::shared_ptr<awst::Expression> _operand)
{
	auto target = buildExpr(m_unaryOp.subExpression());

	// Clear function pointer tracking on delete (e.g., delete y where y is a func ptr)
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_unaryOp.subExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
			m_ctx.funcPtrTargets.erase(varDecl->id());
	}

	if (dynamic_cast<awst::BoxValueExpression const*>(target.get()))
	{
		auto stateDelete = std::make_shared<awst::StateDelete>();
		stateDelete->sourceLocation = m_loc;
		stateDelete->wtype = awst::WType::boolType();
		stateDelete->field = target;
		auto stmt = awst::makeExpressionStatement(std::move(stateDelete), m_loc);
		m_ctx.pendingStatements.push_back(std::move(stmt));
		return _operand;
	}

	// Unwrap ARC4Decode
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		target = decodeExpr->value;

	// ARC4Struct field deletion — copy-on-write with zeroed field
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(target.get()))
	{
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (arc4StructType)
		{
			auto base = fieldExpr->base;
			std::string fieldName = fieldExpr->name;

			auto readBase = base;
			if (dynamic_cast<awst::BoxValueExpression const*>(base.get()))
			{
				auto stateGet = std::make_shared<awst::StateGet>();
				stateGet->sourceLocation = m_loc;
				stateGet->wtype = base->wtype;
				stateGet->field = base;
				stateGet->defaultValue = builder::StorageMapper::makeDefaultValue(base->wtype, m_loc);
				readBase = stateGet;
			}

			awst::WType const* arc4FieldType = nullptr;
			for (auto const& [fname, ftype]: arc4StructType->fields())
				if (fname == fieldName) { arc4FieldType = ftype; break; }

			auto zeroVal = builder::StorageMapper::makeDefaultValue(
				arc4FieldType ? arc4FieldType : fieldExpr->wtype, m_loc);

			auto newStruct = std::make_shared<awst::NewStruct>();
			newStruct->sourceLocation = m_loc;
			newStruct->wtype = arc4StructType;
			for (auto const& [fname, ftype]: arc4StructType->fields())
			{
				if (fname == fieldName)
					newStruct->values[fname] = std::move(zeroVal);
				else
				{
					auto field = std::make_shared<awst::FieldExpression>();
					field->sourceLocation = m_loc;
					field->base = readBase;
					field->name = fname;
					field->wtype = ftype;
					newStruct->values[fname] = std::move(field);
				}
			}

			auto writeTarget = base;
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
				writeTarget = sg->field;

			auto assignStmt = std::make_shared<awst::AssignmentStatement>();
			assignStmt->sourceLocation = m_loc;
			assignStmt->target = std::move(writeTarget);
			assignStmt->value = std::move(newStruct);
			m_ctx.pendingStatements.push_back(std::move(assignStmt));
			return _operand;
		}
	}

	// Default: assign zero value
	auto defaultVal = builder::StorageMapper::makeDefaultValue(target->wtype, m_loc);
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
		target = sg->field;

	// Slot-based storage delete: target is a computed biguint slot (e.g. delete _x[0] on multidim storage array)
	if (dynamic_cast<awst::BigUIntBinaryOperation const*>(target.get())
		&& target->wtype == awst::WType::biguintType())
	{
		// Determine number of slots to clear from the Solidity type
		auto const* subExprType = m_unaryOp.subExpression().annotation().type;
		auto const* arrType = subExprType ? dynamic_cast<ArrayType const*>(subExprType) : nullptr;
		unsigned slotCount = 1;
		if (arrType && !arrType->isDynamicallySized())
			slotCount = static_cast<unsigned>(arrType->length());

		for (unsigned j = 0; j < slotCount; ++j)
		{
			auto jConst = awst::makeIntegerConstant(std::to_string(j), m_loc, awst::WType::biguintType());

			auto slotJ = std::make_shared<awst::BigUIntBinaryOperation>();
			slotJ->sourceLocation = m_loc;
			slotJ->wtype = awst::WType::biguintType();
			slotJ->left = target; // shared, reused
			slotJ->op = awst::BigUIntBinaryOperator::Add;
			slotJ->right = std::move(jConst);

			auto btoi = builder::StorageMapper::biguintSlotToBtoi(slotJ, m_loc);

			auto zeroVal = awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType());

			auto call = std::make_shared<awst::SubroutineCallExpression>();
			call->sourceLocation = m_loc;
			call->wtype = awst::WType::voidType();
			call->target = awst::InstanceMethodTarget{"__storage_write"};
			{
				awst::CallArg slotArg;
				slotArg.name = "__slot";
				slotArg.value = std::move(btoi);
				call->args.push_back(std::move(slotArg));

				awst::CallArg valArg;
				valArg.name = "__value";
				valArg.value = std::move(zeroVal);
				call->args.push_back(std::move(valArg));
			}

			auto stmt = awst::makeExpressionStatement(std::move(call), m_loc);
			m_ctx.pendingStatements.push_back(std::move(stmt));
		}
		return _operand;
	}

	auto assignStmt = std::make_shared<awst::AssignmentStatement>();
	assignStmt->sourceLocation = m_loc;
	assignStmt->target = target;
	assignStmt->value = std::move(defaultVal);
	m_ctx.pendingStatements.push_back(std::move(assignStmt));
	return _operand;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::toAwst()
{
	auto operand = buildExpr(m_unaryOp.subExpression());

	// Try sol-eb builder dispatch for Not/Sub/BitNot
	{
		eb::BuilderUnaryOp builderOp;
		bool hasUnaryOp = true;
		switch (m_unaryOp.getOperator())
		{
		case Token::Not: builderOp = eb::BuilderUnaryOp::LogicalNot; break;
		case Token::Sub: builderOp = eb::BuilderUnaryOp::Negative; break;
		case Token::BitNot: builderOp = eb::BuilderUnaryOp::BitInvert; break;
		default: hasUnaryOp = false; break;
		}
		if (hasUnaryOp)
		{
			auto* solType = m_unaryOp.subExpression().annotation().type;
			auto builder = m_ctx.builderForInstance(solType, operand);
			if (builder)
			{
				auto result = builder->unary_op(builderOp, m_loc);
				if (result)
					return result->resolve();
			}
		}
	}

	switch (m_unaryOp.getOperator())
	{
	case Token::Not:    return handleNot(std::move(operand));
	case Token::Sub:    return handleNegate(std::move(operand));
	case Token::BitNot: return handleBitNot(std::move(operand));
	case Token::Inc:
	case Token::Dec:    return handleIncDec(std::move(operand));
	case Token::Delete: return handleDelete(std::move(operand));
	default:            return operand;
	}
}

} // namespace puyasol::builder::sol_ast
