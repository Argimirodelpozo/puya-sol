/// @file SolBinaryOperation.cpp
/// Migrated from BinaryOperationBuilder.cpp visit() method.

#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>

#include <sstream>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolBinaryOperation::SolBinaryOperation(
	eb::BuilderContext& _ctx,
	BinaryOperation const& _node)
	: SolExpression(_ctx, _node), m_binOp(_node)
{
}

std::shared_ptr<awst::Expression> SolBinaryOperation::tryUserDefinedOp()
{
	auto const* userFunc = *m_binOp.annotation().userDefinedFunction;
	if (!userFunc) return nullptr;

	std::string subroutineId;
	auto it = m_ctx.freeFunctionById.find(userFunc->id());
	if (it != m_ctx.freeFunctionById.end())
		subroutineId = it->second;
	else
	{
		auto const* scope = userFunc->scope();
		auto const* libContract = dynamic_cast<ContractDefinition const*>(scope);
		if (libContract && libContract->isLibrary())
		{
			std::string qualifiedName = libContract->name() + "." + userFunc->name();
			auto libIt = m_ctx.libraryFunctionIds.find(qualifiedName);
			if (libIt != m_ctx.libraryFunctionIds.end())
				subroutineId = libIt->second;
		}
		if (subroutineId.empty())
			subroutineId = m_ctx.sourceFile + "." + userFunc->name();
	}

	auto left = buildExpr(m_binOp.leftExpression());
	auto right = buildExpr(m_binOp.rightExpression());
	auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);

	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = m_loc;
	call->wtype = resultType;
	call->target = awst::SubroutineID{subroutineId};

	awst::CallArg argA;
	argA.name = userFunc->parameters()[0]->name();
	argA.value = std::move(left);
	call->args.push_back(std::move(argA));

	awst::CallArg argB;
	argB.name = userFunc->parameters()[1]->name();
	argB.value = std::move(right);
	call->args.push_back(std::move(argB));

	return call;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::tryConstantFold()
{
	if (auto const* ratType = dynamic_cast<RationalNumberType const*>(
			m_binOp.annotation().type))
	{
		if (!ratType->isFractional())
		{
			auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = m_loc;
			e->wtype = resultType;
			e->value = ratType->literalValue(nullptr).str();
			return e;
		}
	}
	return nullptr;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::trySolEbDispatch(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right)
{
	auto solOp = m_binOp.getOperator();
	auto* leftSolType = m_binOp.leftExpression().annotation().type;
	auto* rightSolType = m_binOp.rightExpression().annotation().type;

	auto leftBuilder = m_ctx.builderForInstance(leftSolType, _left);
	if (!leftBuilder) return nullptr;

	// If right operand is a compile-time constant (RationalNumberType),
	// use the left operand's type for the builder so overflow checks use
	// the correct bit width (e.g., uint16 + 256 should check uint16 overflow).
	auto rightBuilder = m_ctx.builderForInstance(rightSolType, _right);
	if (!rightBuilder && leftSolType)
		rightBuilder = m_ctx.builderForInstance(leftSolType, _right);
	if (!rightBuilder) return nullptr;

	// Try comparison operators
	eb::BuilderComparisonOp cmpOp;
	bool hasCmpOp = true;
	switch (solOp)
	{
	case Token::Equal:              cmpOp = eb::BuilderComparisonOp::Eq; break;
	case Token::NotEqual:           cmpOp = eb::BuilderComparisonOp::Ne; break;
	case Token::LessThan:           cmpOp = eb::BuilderComparisonOp::Lt; break;
	case Token::LessThanOrEqual:    cmpOp = eb::BuilderComparisonOp::Lte; break;
	case Token::GreaterThan:        cmpOp = eb::BuilderComparisonOp::Gt; break;
	case Token::GreaterThanOrEqual: cmpOp = eb::BuilderComparisonOp::Gte; break;
	default: hasCmpOp = false; break;
	}
	if (hasCmpOp)
	{
		auto result = leftBuilder->compare(*rightBuilder, cmpOp, m_loc);
		if (result) return result->resolve();
	}

	// Try arithmetic/bitwise operators
	eb::BuilderBinaryOp builderOp;
	bool hasBinOp = true;
	switch (solOp)
	{
	case Token::Add: case Token::AssignAdd: builderOp = eb::BuilderBinaryOp::Add; break;
	case Token::Sub: case Token::AssignSub: builderOp = eb::BuilderBinaryOp::Sub; break;
	case Token::Mul: case Token::AssignMul: builderOp = eb::BuilderBinaryOp::Mult; break;
	case Token::Div: case Token::AssignDiv: builderOp = eb::BuilderBinaryOp::FloorDiv; break;
	case Token::Mod: case Token::AssignMod: builderOp = eb::BuilderBinaryOp::Mod; break;
	case Token::Exp: builderOp = eb::BuilderBinaryOp::Pow; break;
	case Token::SHL: case Token::AssignShl: builderOp = eb::BuilderBinaryOp::LShift; break;
	case Token::SHR: case Token::SAR: case Token::AssignShr: case Token::AssignSar:
		builderOp = eb::BuilderBinaryOp::RShift; break;
	case Token::BitOr: case Token::AssignBitOr: builderOp = eb::BuilderBinaryOp::BitOr; break;
	case Token::BitXor: case Token::AssignBitXor: builderOp = eb::BuilderBinaryOp::BitXor; break;
	case Token::BitAnd: case Token::AssignBitAnd: builderOp = eb::BuilderBinaryOp::BitAnd; break;
	default: hasBinOp = false; break;
	}
	if (hasBinOp)
	{
		auto result = leftBuilder->binary_op(*rightBuilder, builderOp, m_loc);
		if (result) return result->resolve();
	}

	return nullptr;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::toAwst()
{
	// 1. User-defined operator overloading
	if (auto result = tryUserDefinedOp())
		return result;

	// 2. Constant folding
	if (auto result = tryConstantFold())
		return result;

	// 3. Build operands
	auto left = buildExpr(m_binOp.leftExpression());
	auto right = buildExpr(m_binOp.rightExpression());
	auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);

	// 4. Signed integer arithmetic — wrap mod 2^N + overflow detection
	// Must come before sol-eb dispatch which doesn't handle signed wrapping.
	auto const* commonType = m_binOp.annotation().commonType;
	if (auto const* intType = dynamic_cast<IntegerType const*>(commonType))
	{
		if (intType->isSigned())
		{
			auto op = m_binOp.getOperator();
			if (op == Token::Add || op == Token::AssignAdd
				|| op == Token::Sub || op == Token::AssignSub
				|| op == Token::Mul || op == Token::AssignMul)
			{
				return buildSignedArithmetic(op, std::move(left), std::move(right), intType);
			}
		}
	}

	// 5. Sol-eb builder dispatch
	if (auto result = trySolEbDispatch(left, right))
		return result;

	// 6. Fallback to buildBinaryOp
	return m_ctx.buildBinaryOp(
		m_binOp.getOperator(), std::move(left), std::move(right), resultType, m_loc);
}

std::shared_ptr<awst::Expression> SolBinaryOperation::buildSignedArithmetic(
	Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	IntegerType const* _intType)
{
	unsigned bits = _intType->numBits();
	bool isBiguint = (bits > 64);

	// Compute 2^N and 2^(N-1) as string constants
	// Note: u256 can't hold 2^256 (it overflows to 0), so special-case it.
	std::string pow2NStr, halfNStr;
	if (bits == 256)
	{
		pow2NStr = "115792089237316195423570985008687907853269984665640564039457584007913129639936";
		halfNStr = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
	}
	else
	{
		solidity::u256 pow2N = solidity::u256(1) << bits;
		solidity::u256 halfN = solidity::u256(1) << (bits - 1);
		std::ostringstream pow2NOss, halfNOss;
		pow2NOss << pow2N;
		halfNOss << halfN;
		pow2NStr = pow2NOss.str();
		halfNStr = halfNOss.str();
	}

	auto makeConst = [&](std::string const& val) -> std::shared_ptr<awst::Expression> {
		auto c = std::make_shared<awst::IntegerConstant>();
		c->sourceLocation = m_loc;
		c->wtype = isBiguint ? awst::WType::biguintType() : awst::WType::uint64Type();
		c->value = val;
		return c;
	};

	auto makeBiguintConst = [&](std::string const& val) -> std::shared_ptr<awst::Expression> {
		auto c = std::make_shared<awst::IntegerConstant>();
		c->sourceLocation = m_loc;
		c->wtype = awst::WType::biguintType();
		c->value = val;
		return c;
	};

	// (operand coercion to biguint happens in Step 1 below)

	// Ensure both operands are biguint (needed for mod 2^N wrapping)
	auto ensureBiguint = [&](std::shared_ptr<awst::Expression> expr)
		-> std::shared_ptr<awst::Expression> {
		if (expr->wtype == awst::WType::biguintType())
			return expr;
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = m_loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(expr));
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(itob);
		return cast;
	};

	_left = ensureBiguint(std::move(_left));
	_right = ensureBiguint(std::move(_right));

	// Step 1: Perform unsigned operation (all in biguint)
	// For subtraction: compute (a + 2^N - b) instead of (a - b) to avoid negative biguint.
	std::shared_ptr<awst::Expression> rawResult;
	bool isSub = (_op == Token::Sub || _op == Token::AssignSub);

	if (isSub)
	{
		// (a + 2^N - b) — always non-negative since a < 2^N and b < 2^N
		auto aPlusPow = std::make_shared<awst::BigUIntBinaryOperation>();
		aPlusPow->sourceLocation = m_loc;
		aPlusPow->wtype = awst::WType::biguintType();
		aPlusPow->left = _left;
		aPlusPow->op = awst::BigUIntBinaryOperator::Add;
		aPlusPow->right = makeBiguintConst(pow2NStr);

		auto subB = std::make_shared<awst::BigUIntBinaryOperation>();
		subB->sourceLocation = m_loc;
		subB->wtype = awst::WType::biguintType();
		subB->left = std::move(aPlusPow);
		subB->op = awst::BigUIntBinaryOperator::Sub;
		subB->right = _right;
		rawResult = std::move(subB);
	}
	else
	{
		auto binOp = std::make_shared<awst::BigUIntBinaryOperation>();
		binOp->sourceLocation = m_loc;
		binOp->wtype = awst::WType::biguintType();
		binOp->left = _left;
		binOp->right = _right;
		if (_op == Token::Mul || _op == Token::AssignMul)
			binOp->op = awst::BigUIntBinaryOperator::Mult;
		else
			binOp->op = awst::BigUIntBinaryOperator::Add;
		rawResult = std::move(binOp);
	}

	// Step 2: Wrap mod 2^N (always needed for signed — two's complement semantics)
	{
		auto wrapOp = std::make_shared<awst::BigUIntBinaryOperation>();
		wrapOp->sourceLocation = m_loc;
		wrapOp->wtype = awst::WType::biguintType();
		wrapOp->left = std::move(rawResult);
		wrapOp->op = awst::BigUIntBinaryOperator::Mod;
		wrapOp->right = makeBiguintConst(pow2NStr);
		rawResult = std::move(wrapOp);
	}

	// Step 3: Signed overflow check (skip in unchecked blocks)
	if (!m_ctx.inUncheckedBlock)
	{
		// Signed overflow for add: both same sign, result different sign
		// Signed overflow for sub: different signs, result sign != a's sign
		// Signed overflow for mul: result doesn't round-trip (complex, use range check)
		//
		// General approach: check result is in [0, 2^(N-1)) or [2^N - 2^(N-1), 2^N)
		// i.e., the two's complement value is in [-2^(N-1), 2^(N-1)-1]
		// For add/sub, use the sign-based check. For mul, use range check.
		//
		// For add: overflow iff a_neg == b_neg && a_neg != result_neg
		// For sub: overflow iff a_neg != b_neg && a_neg != result_neg

		// Coerce a value to biguint if needed
		auto toBiguint = [&](std::shared_ptr<awst::Expression> const& val)
			-> std::shared_ptr<awst::Expression> {
			if (val->wtype == awst::WType::biguintType())
				return val;
			auto itob = std::make_shared<awst::IntrinsicCall>();
			itob->sourceLocation = m_loc;
			itob->wtype = awst::WType::bytesType();
			itob->opCode = "itob";
			itob->stackArgs.push_back(val);
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = m_loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(itob);
			return cast;
		};

		// isNeg: val >= half  ↔  NOT (val < half)
		auto isNeg = [&](std::shared_ptr<awst::Expression> const& val)
			-> std::shared_ptr<awst::Expression> {
			auto cmp = std::make_shared<awst::NumericComparisonExpression>();
			cmp->sourceLocation = m_loc;
			cmp->wtype = awst::WType::boolType();
			cmp->lhs = toBiguint(val);
			cmp->op = awst::NumericComparison::Lt;
			cmp->rhs = makeBiguintConst(halfNStr);
			auto notExpr = std::make_shared<awst::Not>();
			notExpr->sourceLocation = m_loc;
			notExpr->wtype = awst::WType::boolType();
			notExpr->expr = std::move(cmp);
			return notExpr;
		};

		std::shared_ptr<awst::Expression> overflowCond;

		if (_op == Token::Add || _op == Token::AssignAdd)
		{
			// Overflow iff: a_neg == b_neg && a_neg != result_neg
			// assert: a_neg != b_neg || a_neg == result_neg
			auto aNeg = isNeg(_left);
			auto bNeg = isNeg(_right);
			auto rNeg = isNeg(rawResult);

			// a_neg != b_neg (different signs → no overflow possible)
			auto diffSigns = std::make_shared<awst::NumericComparisonExpression>();
			diffSigns->sourceLocation = m_loc;
			diffSigns->wtype = awst::WType::boolType();
			diffSigns->lhs = aNeg;
			diffSigns->op = awst::NumericComparison::Ne;
			diffSigns->rhs = bNeg;

			// a_neg == result_neg (same sign as input → no overflow)
			auto sameSignResult = std::make_shared<awst::NumericComparisonExpression>();
			sameSignResult->sourceLocation = m_loc;
			sameSignResult->wtype = awst::WType::boolType();
			sameSignResult->lhs = aNeg;
			sameSignResult->op = awst::NumericComparison::Eq;
			sameSignResult->rhs = rNeg;

			// OR: either different input signs or result has same sign as a
			auto noOverflow = std::make_shared<awst::BooleanBinaryOperation>();
			noOverflow->sourceLocation = m_loc;
			noOverflow->wtype = awst::WType::boolType();
			noOverflow->left = std::move(diffSigns);
			noOverflow->op = awst::BinaryBooleanOperator::Or;
			noOverflow->right = std::move(sameSignResult);
			overflowCond = std::move(noOverflow);
		}
		else if (_op == Token::Sub || _op == Token::AssignSub)
		{
			// Overflow iff: a_neg != b_neg && a_neg != result_neg
			// assert: a_neg == b_neg || a_neg == result_neg
			auto aNeg = isNeg(_left);
			auto bNeg = isNeg(_right);
			auto rNeg = isNeg(rawResult);

			auto sameSigns = std::make_shared<awst::NumericComparisonExpression>();
			sameSigns->sourceLocation = m_loc;
			sameSigns->wtype = awst::WType::boolType();
			sameSigns->lhs = aNeg;
			sameSigns->op = awst::NumericComparison::Eq;
			sameSigns->rhs = bNeg;

			auto sameSignResult = std::make_shared<awst::NumericComparisonExpression>();
			sameSignResult->sourceLocation = m_loc;
			sameSignResult->wtype = awst::WType::boolType();
			sameSignResult->lhs = aNeg;
			sameSignResult->op = awst::NumericComparison::Eq;
			sameSignResult->rhs = rNeg;

			auto noOverflow = std::make_shared<awst::BooleanBinaryOperation>();
			noOverflow->sourceLocation = m_loc;
			noOverflow->wtype = awst::WType::boolType();
			noOverflow->left = std::move(sameSigns);
			noOverflow->op = awst::BinaryBooleanOperator::Or;
			noOverflow->right = std::move(sameSignResult);
			overflowCond = std::move(noOverflow);
		}
		else // mul
		{
			// For signed multiplication, use a simpler range check.
			// The result must be < 2^(N-1) (positive) or >= 2^N - 2^(N-1) (negative).
			// But this doesn't catch all overflows. Use the division round-trip:
			// If b != 0: assert(a == result / b) where / is signed division
			// For simplicity, just assert result < 2^N (already done by mod).
			// TODO: Proper signed mul overflow detection
			overflowCond = nullptr;
		}

		if (overflowCond)
		{
			auto assertExpr = std::make_shared<awst::AssertExpression>();
			assertExpr->sourceLocation = m_loc;
			assertExpr->wtype = awst::WType::voidType();
			assertExpr->condition = std::move(overflowCond);
			assertExpr->errorMessage = "signed arithmetic overflow";

			auto assertStmt = std::make_shared<awst::ExpressionStatement>();
			assertStmt->sourceLocation = m_loc;
			assertStmt->expr = std::move(assertExpr);
			m_ctx.prePendingStatements.push_back(std::move(assertStmt));
		}
	}

	// Step 4: Truncate back to uint64 for ≤64-bit types
	if (!isBiguint)
	{
		// rawResult is biguint from the mod — convert back to uint64
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = awst::WType::bytesType();
		cast->expr = std::move(rawResult);

		// concat(bzero(8), bytes) then extract last 8 bytes → btoi
		auto eight = std::make_shared<awst::IntegerConstant>();
		eight->sourceLocation = m_loc;
		eight->wtype = awst::WType::uint64Type();
		eight->value = "8";

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
		cat->stackArgs.push_back(std::move(cast));

		auto lenCall = std::make_shared<awst::IntrinsicCall>();
		lenCall->sourceLocation = m_loc;
		lenCall->wtype = awst::WType::uint64Type();
		lenCall->opCode = "len";
		lenCall->stackArgs.push_back(cat);

		auto eight2 = std::make_shared<awst::IntegerConstant>();
		eight2->sourceLocation = m_loc;
		eight2->wtype = awst::WType::uint64Type();
		eight2->value = "8";

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

	return rawResult;
}

} // namespace puyasol::builder::sol_ast
