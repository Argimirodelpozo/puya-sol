/// @file SolExpressionStatement.cpp
/// ExpressionStatement, RevertStatement, ReturnStatement.

#include "builder/sol-ast/stmts/SolExpressionStatement.h"
#include "builder/ExpressionBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

// ── ExpressionStatement ──

SolExpressionStatement::SolExpressionStatement(
	StatementContext& _ctx, ExpressionStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolExpressionStatement::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	auto expr = m_ctx.buildExpr(m_node.expression());

	for (auto& p: m_ctx.takePrePending())
		result.push_back(std::move(p));

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = m_loc;
	stmt->expr = std::move(expr);
	result.push_back(stmt);

	for (auto& p: m_ctx.takePending())
		result.push_back(std::move(p));

	return result;
}

// ── RevertStatement ──

SolRevertStatement::SolRevertStatement(
	StatementContext& _ctx, RevertStatement const& _node, awst::SourceLocation _loc)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolRevertStatement::toAwst()
{
	std::string errorName = "revert";
	auto const& errorCall = m_node.errorCall();
	if (auto const* ident = dynamic_cast<Identifier const*>(&errorCall.expression()))
		errorName = ident->name();
	else if (auto const* ma = dynamic_cast<MemberAccess const*>(&errorCall.expression()))
		errorName = ma->memberName();

	auto assertExpr = std::make_shared<awst::AssertExpression>();
	assertExpr->sourceLocation = m_loc;
	assertExpr->wtype = awst::WType::voidType();
	auto falseLit = std::make_shared<awst::BoolConstant>();
	falseLit->sourceLocation = m_loc;
	falseLit->wtype = awst::WType::boolType();
	falseLit->value = false;
	assertExpr->condition = falseLit;
	assertExpr->errorMessage = errorName;

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = m_loc;
	stmt->expr = assertExpr;
	return {stmt};
}

// ── ReturnStatement ──

SolReturnStatement::SolReturnStatement(
	StatementContext& _ctx, Return const& _node, awst::SourceLocation _loc)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolReturnStatement::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;

	auto stmt = std::make_shared<awst::ReturnStatement>();
	stmt->sourceLocation = m_loc;

	if (m_node.expression())
	{
		stmt->value = m_ctx.buildExpr(*m_node.expression());

		auto const& retAnnotation = dynamic_cast<ReturnAnnotation const&>(m_node.annotation());
		if (retAnnotation.functionReturnParameters)
		{
			auto const& retParams = retAnnotation.functionReturnParameters->parameters();
			if (retParams.size() == 1)
			{
				auto* expectedType = m_ctx.typeMapper->map(retParams[0]->type());
				stmt->value = ExpressionBuilder::implicitNumericCast(
					std::move(stmt->value), expectedType, m_loc);

				// IntegerConstant → BytesConstant for bytes[N] returns
				if (expectedType && expectedType->kind() == awst::WTypeKind::Bytes
					&& stmt->value->wtype != expectedType)
				{
					auto const* bytesType = dynamic_cast<awst::BytesWType const*>(expectedType);
					auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(stmt->value.get());
					if (bytesType && intConst && bytesType->length().value_or(0) > 0)
					{
						int numBytes = bytesType->length().value();
						uint64_t val = std::stoull(intConst->value);
						std::vector<unsigned char> bytes(numBytes, 0);
						for (int i = numBytes - 1; i >= 0 && val > 0; --i)
						{
							bytes[i] = static_cast<unsigned char>(val & 0xFF);
							val >>= 8;
						}
						auto bc = std::make_shared<awst::BytesConstant>();
						bc->sourceLocation = m_loc;
						bc->wtype = expectedType;
						bc->encoding = awst::BytesEncoding::Base16;
						bc->value = bytes;
						stmt->value = std::move(bc);
					}
					else if (auto const* strConst = dynamic_cast<awst::StringConstant const*>(stmt->value.get()))
					{
						// StringConstant → BytesConstant(bytes[N]): right-pad with zeros
						int numBytes = bytesType ? bytesType->length().value_or(0) : 0;
						if (numBytes > 0)
						{
							std::vector<unsigned char> bytes(numBytes, 0);
							auto const& s = strConst->value;
							for (size_t i = 0; i < s.size() && i < bytes.size(); ++i)
								bytes[i] = static_cast<unsigned char>(s[i]);
							auto bc = std::make_shared<awst::BytesConstant>();
							bc->sourceLocation = m_loc;
							bc->wtype = expectedType;
							bc->encoding = awst::BytesEncoding::Base16;
							bc->value = bytes;
							stmt->value = std::move(bc);
						}
					}
					else if (stmt->value->wtype && stmt->value->wtype->kind() == awst::WTypeKind::Bytes)
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = m_loc;
						cast->wtype = expectedType;
						cast->expr = std::move(stmt->value);
						stmt->value = std::move(cast);
					}
					else if (stmt->value->wtype == awst::WType::stringType())
					{
						// String → bytes[N]: right-pad
						int numBytes = bytesType ? bytesType->length().value_or(0) : 0;
						if (numBytes > 0)
						{
							auto padded = builder::TypeCoercion::stringToBytesN(
								stmt->value.get(), expectedType, numBytes, m_loc);
							if (padded)
								stmt->value = std::move(padded);
						}
					}
				}
			}
			else if (retParams.size() > 1)
			{
				auto* tupleExpr = dynamic_cast<awst::TupleExpression*>(stmt->value.get());
				if (tupleExpr && tupleExpr->items.size() == retParams.size())
				{
					std::vector<awst::WType const*> expectedTypes;
					for (size_t i = 0; i < retParams.size(); ++i)
					{
						auto* expectedElemType = m_ctx.typeMapper->map(retParams[i]->type());
						tupleExpr->items[i] = ExpressionBuilder::implicitNumericCast(
							std::move(tupleExpr->items[i]), expectedElemType, m_loc);
						expectedTypes.push_back(tupleExpr->items[i]->wtype);
					}
					tupleExpr->wtype = m_ctx.typeMapper->createType<awst::WTuple>(
						std::move(expectedTypes), std::nullopt);
				}
			}
		}
	}

	for (auto& p: m_ctx.takePrePending())
		result.push_back(std::move(p));
	for (auto& p: m_ctx.takePending())
		result.push_back(std::move(p));

	result.push_back(stmt);
	return result;
}

} // namespace puyasol::builder::sol_ast
