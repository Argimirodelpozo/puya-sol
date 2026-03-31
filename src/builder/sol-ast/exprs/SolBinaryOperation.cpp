/// @file SolBinaryOperation.cpp
/// Migrated from BinaryOperationBuilder.cpp visit() method.

#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

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

	auto rightBuilder = m_ctx.builderForInstance(rightSolType, _right);
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

	// 4. Sol-eb builder dispatch
	if (auto result = trySolEbDispatch(left, right))
		return result;

	// 5. Fallback to buildBinaryOp
	return m_ctx.buildBinaryOp(
		m_binOp.getOperator(), std::move(left), std::move(right), resultType, m_loc);
}

} // namespace puyasol::builder::sol_ast
