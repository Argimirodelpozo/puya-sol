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

	// Type expressions as statements (e.g. `s[7][];`) resolve to a type
	// value with no runtime representation. We still need to walk the
	// expression tree to pick up side effects (e.g. `((flag = true) ? M : M).D;`
	// needs the assignment to happen) but we must not emit the final value
	// expression because our type mapper can't model it.
	bool isTypeType = dynamic_cast<solidity::frontend::TypeType const*>(
		m_node.expression().annotation().type) != nullptr;

	auto expr = m_ctx.buildExpr(m_node.expression());

	for (auto& p: m_ctx.takePrePending())
		result.push_back(std::move(p));

	// If buildExpr couldn't produce a value expression, or the expression
	// is a type-valued expression, skip emitting the final statement to
	// avoid a null dereference or invalid AWST.
	if (!expr || isTypeType)
	{
		for (auto& p: m_ctx.takePending())
			result.push_back(std::move(p));
		return result;
	}

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

	if (!m_node.expression())
	{
		// Bare return; — synthesize a return value from context
		auto const& retAnnotation = dynamic_cast<ReturnAnnotation const&>(m_node.annotation());
		if (retAnnotation.functionReturnParameters)
		{
			auto const& retParams = retAnnotation.functionReturnParameters->parameters();
			if (retParams.size() == 1)
			{
				auto* retType = m_ctx.typeMapper->map(retParams[0]->type());
				if (!retParams[0]->name().empty())
				{
					// Named return: return the variable
					auto retVar = std::make_shared<awst::VarExpression>();
					retVar->sourceLocation = m_loc;
					retVar->wtype = retType;
					retVar->name = retParams[0]->name();
					stmt->value = std::move(retVar);
				}
				else
				{
					// Unnamed return: return default value (0/false/empty)
					stmt->value = builder::StorageMapper::makeDefaultValue(retType, m_loc);
				}
			}
		}
	}
	else if (m_node.expression())
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

				// Sign-extend if returning a narrow signed integer as a wider signed type.
				// e.g. int8 result returned as int256: value 128 (int8 -128) needs
				// to become 2^256-128 (int256 -128 in two's complement).
				auto const* exprSolType = m_node.expression()->annotation().type;
				auto const* retSolType = retParams[0]->type();
				if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(exprSolType))
					exprSolType = &udvt->underlyingType();
				if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(retSolType))
					retSolType = &udvt->underlyingType();
				auto const* exprInt = dynamic_cast<solidity::frontend::IntegerType const*>(exprSolType);
				auto const* retInt = dynamic_cast<solidity::frontend::IntegerType const*>(retSolType);
				if (exprInt && retInt && exprInt->isSigned() && retInt->isSigned()
					&& exprInt->numBits() < retInt->numBits())
				{
					stmt->value = TypeCoercion::signExtendToUint256(
						std::move(stmt->value), exprInt->numBits(), m_loc);
				}

				// Array type conversion: dynamic↔static
				if (stmt->value->wtype && expectedType
					&& stmt->value->wtype != expectedType
					&& ((stmt->value->wtype->kind() == awst::WTypeKind::ARC4DynamicArray
						&& expectedType->kind() == awst::WTypeKind::ARC4StaticArray)
					|| (stmt->value->wtype->kind() == awst::WTypeKind::ARC4StaticArray
						&& expectedType->kind() == awst::WTypeKind::ARC4DynamicArray)))
				{
					auto convert = std::make_shared<awst::ConvertArray>();
					convert->sourceLocation = m_loc;
					convert->wtype = expectedType;
					convert->expr = std::move(stmt->value);
					stmt->value = std::move(convert);
				}

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
						// Bytes type widening: e.g. bytes2 → bytes16
						if (tupleExpr->items[i]->wtype != expectedElemType
							&& tupleExpr->items[i]->wtype
							&& tupleExpr->items[i]->wtype->kind() == awst::WTypeKind::Bytes
							&& expectedElemType->kind() == awst::WTypeKind::Bytes)
						{
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = m_loc;
							cast->wtype = expectedElemType;
							cast->expr = std::move(tupleExpr->items[i]);
							tupleExpr->items[i] = std::move(cast);
						}
						expectedTypes.push_back(tupleExpr->items[i]->wtype);
					}
					tupleExpr->wtype = m_ctx.typeMapper->createType<awst::WTuple>(
						std::move(expectedTypes), std::nullopt);
				}
				else if (stmt->value->wtype
					&& stmt->value->wtype->kind() == awst::WTypeKind::WTuple)
				{
					// Non-tuple-literal returning a tuple (e.g., conditional expression).
					// Coerce branches of conditional expressions element-by-element.
					auto* condExpr = dynamic_cast<awst::ConditionalExpression*>(stmt->value.get());
					if (condExpr)
					{
						std::vector<awst::WType const*> expectedTypes;
						for (size_t i = 0; i < retParams.size(); ++i)
							expectedTypes.push_back(m_ctx.typeMapper->map(retParams[i]->type()));
						auto* expectedTupleType = m_ctx.typeMapper->createType<awst::WTuple>(
							std::vector<awst::WType const*>(expectedTypes), std::nullopt);

						// Coerce true branch
						auto* trueTuple = dynamic_cast<awst::TupleExpression*>(condExpr->trueExpr.get());
						if (trueTuple && trueTuple->items.size() == retParams.size())
						{
							for (size_t i = 0; i < retParams.size(); ++i)
								trueTuple->items[i] = ExpressionBuilder::implicitNumericCast(
									std::move(trueTuple->items[i]), expectedTypes[i], m_loc);
							trueTuple->wtype = expectedTupleType;
						}
						// Coerce false branch
						auto* falseTuple = dynamic_cast<awst::TupleExpression*>(condExpr->falseExpr.get());
						if (falseTuple && falseTuple->items.size() == retParams.size())
						{
							for (size_t i = 0; i < retParams.size(); ++i)
								falseTuple->items[i] = ExpressionBuilder::implicitNumericCast(
									std::move(falseTuple->items[i]), expectedTypes[i], m_loc);
							falseTuple->wtype = expectedTupleType;
						}
						condExpr->wtype = expectedTupleType;
					}
				}
			}
		}
	}

	for (auto& p: m_ctx.takePrePending())
		result.push_back(std::move(p));
	for (auto& p: m_ctx.takePending())
		result.push_back(std::move(p));

	// Enum range validation on return: EVM panics (0x21) on invalid enum return values
	if (stmt->value)
	{
		auto const& retAnnotation = dynamic_cast<ReturnAnnotation const&>(m_node.annotation());
		if (retAnnotation.functionReturnParameters)
		{
			auto const& retParams = retAnnotation.functionReturnParameters->parameters();
			if (retParams.size() == 1)
			{
				if (auto const* enumType = dynamic_cast<EnumType const*>(retParams[0]->type()))
				{
					unsigned numMembers = enumType->numberOfMembers();
					auto val = builder::TypeCoercion::implicitNumericCast(
						stmt->value, awst::WType::uint64Type(), m_loc);

					auto maxVal = std::make_shared<awst::IntegerConstant>();
					maxVal->sourceLocation = m_loc;
					maxVal->wtype = awst::WType::uint64Type();
					maxVal->value = std::to_string(numMembers);

					auto cmp = std::make_shared<awst::NumericComparisonExpression>();
					cmp->sourceLocation = m_loc;
					cmp->wtype = awst::WType::boolType();
					cmp->lhs = val;
					cmp->op = awst::NumericComparison::Lt;
					cmp->rhs = std::move(maxVal);

					auto assertExpr = std::make_shared<awst::AssertExpression>();
					assertExpr->sourceLocation = m_loc;
					assertExpr->wtype = awst::WType::voidType();
					assertExpr->condition = std::move(cmp);
					assertExpr->errorMessage = "enum out of range";

					auto assertStmt = std::make_shared<awst::ExpressionStatement>();
					assertStmt->sourceLocation = m_loc;
					assertStmt->expr = std::move(assertExpr);
					result.push_back(std::move(assertStmt));
				}
			}
		}
	}

	result.push_back(stmt);
	return result;
}

} // namespace puyasol::builder::sol_ast
