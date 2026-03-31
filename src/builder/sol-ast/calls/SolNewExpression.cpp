/// @file SolNewExpression.cpp
/// new bytes(N), new T[](N), new Contract(...).
/// Migrated from FunctionCallBuilder.cpp lines 2144-2304.

#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolNewExpression::handleNewBytes()
{
	auto* resultType = m_ctx.typeMapper.map(m_call.annotation().type);
	auto sizeExpr = !m_call.arguments().empty()
		? buildExpr(*m_call.arguments()[0])
		: nullptr;
	if (sizeExpr)
		sizeExpr = builder::TypeCoercion::implicitNumericCast(
			std::move(sizeExpr), awst::WType::uint64Type(), m_loc);

	auto e = std::make_shared<awst::IntrinsicCall>();
	e->sourceLocation = m_loc;
	e->wtype = resultType;
	e->opCode = "bzero";
	if (sizeExpr)
		e->stackArgs.push_back(std::move(sizeExpr));
	return e;
}

std::shared_ptr<awst::Expression> SolNewExpression::handleNewArray()
{
	auto* resultType = m_ctx.typeMapper.map(m_call.annotation().type);
	auto* refArr = dynamic_cast<awst::ReferenceArray const*>(resultType);
	auto e = std::make_shared<awst::NewArray>();
	e->sourceLocation = m_loc;
	e->wtype = resultType;

	if (!m_call.arguments().empty() && refArr)
	{
		// Try compile-time size resolution
		unsigned long long n = 0;
		auto const* argType = m_call.arguments()[0]->annotation().type;
		if (auto const* ratType = dynamic_cast<RationalNumberType const*>(argType))
		{
			auto val = ratType->literalValue(nullptr);
			if (val > 0)
				n = static_cast<unsigned long long>(val);
		}
		// Try tracked constant locals
		if (n == 0)
		{
			if (auto const* ident = dynamic_cast<Identifier const*>(&*m_call.arguments()[0]))
			{
				auto it = m_ctx.constantLocals.find(
					ident->annotation().referencedDeclaration->id());
				if (it != m_ctx.constantLocals.end() && it->second > 0)
					n = it->second;
			}
		}

		if (n > 0)
		{
			// Compile-time known: N default values
			for (unsigned long long i = 0; i < n; ++i)
				e->values.push_back(
					builder::StorageMapper::makeDefaultValue(refArr->elementType(), m_loc));
		}
		else
		{
			// Runtime-sized: loop pattern
			static int rtArrayCounter = 0;
			int tc = rtArrayCounter++;
			std::string arrName = "__rt_arr_" + std::to_string(tc);
			std::string idxName = "__rt_idx_" + std::to_string(tc);

			auto sizeExpr = buildExpr(*m_call.arguments()[0]);
			sizeExpr = builder::TypeCoercion::implicitNumericCast(
				std::move(sizeExpr), awst::WType::uint64Type(), m_loc);

			// __arr = NewArray()
			auto arrVar = std::make_shared<awst::VarExpression>();
			arrVar->sourceLocation = m_loc;
			arrVar->wtype = resultType;
			arrVar->name = arrName;

			auto initArr = std::make_shared<awst::AssignmentStatement>();
			initArr->sourceLocation = m_loc;
			initArr->target = arrVar;
			initArr->value = e;
			m_ctx.prePendingStatements.push_back(std::move(initArr));

			// __i = 0
			auto idxVar = std::make_shared<awst::VarExpression>();
			idxVar->sourceLocation = m_loc;
			idxVar->wtype = awst::WType::uint64Type();
			idxVar->name = idxName;

			auto initIdx = std::make_shared<awst::AssignmentStatement>();
			initIdx->sourceLocation = m_loc;
			initIdx->target = idxVar;
			auto zero = std::make_shared<awst::IntegerConstant>();
			zero->sourceLocation = m_loc;
			zero->wtype = awst::WType::uint64Type();
			zero->value = "0";
			initIdx->value = zero;
			m_ctx.prePendingStatements.push_back(std::move(initIdx));

			// while (__i < n)
			auto loop = std::make_shared<awst::WhileLoop>();
			loop->sourceLocation = m_loc;
			auto cond = std::make_shared<awst::NumericComparisonExpression>();
			cond->sourceLocation = m_loc;
			cond->wtype = awst::WType::boolType();
			cond->lhs = idxVar;
			cond->op = awst::NumericComparison::Lt;
			cond->rhs = sizeExpr;
			loop->condition = cond;

			auto loopBody = std::make_shared<awst::Block>();
			loopBody->sourceLocation = m_loc;

			// extend with default
			auto defaultElem = builder::StorageMapper::makeDefaultValue(refArr->elementType(), m_loc);
			auto singleArr = std::make_shared<awst::NewArray>();
			singleArr->sourceLocation = m_loc;
			singleArr->wtype = resultType;
			singleArr->values.push_back(std::move(defaultElem));

			auto extend = std::make_shared<awst::ArrayExtend>();
			extend->sourceLocation = m_loc;
			extend->wtype = awst::WType::voidType();
			extend->base = arrVar;
			extend->other = std::move(singleArr);
			auto extendStmt = std::make_shared<awst::ExpressionStatement>();
			extendStmt->sourceLocation = m_loc;
			extendStmt->expr = extend;
			loopBody->body.push_back(std::move(extendStmt));

			// __i++
			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = m_loc;
			one->wtype = awst::WType::uint64Type();
			one->value = "1";
			auto incr = std::make_shared<awst::UInt64BinaryOperation>();
			incr->sourceLocation = m_loc;
			incr->wtype = awst::WType::uint64Type();
			incr->left = idxVar;
			incr->op = awst::UInt64BinaryOperator::Add;
			incr->right = one;
			auto incrAssign = std::make_shared<awst::AssignmentStatement>();
			incrAssign->sourceLocation = m_loc;
			incrAssign->target = idxVar;
			incrAssign->value = incr;
			loopBody->body.push_back(std::move(incrAssign));

			loop->loopBody = loopBody;
			m_ctx.prePendingStatements.push_back(std::move(loop));

			return arrVar;
		}
	}

	return e;
}

std::shared_ptr<awst::Expression> SolNewExpression::toAwst()
{
	auto* resultType = m_ctx.typeMapper.map(m_call.annotation().type);

	if (resultType && resultType->kind() == awst::WTypeKind::Bytes)
		return handleNewBytes();

	if (resultType && resultType->kind() == awst::WTypeKind::ReferenceArray)
		return handleNewArray();

	// new Contract(...) — not yet supported (needs two-pass compilation)
	auto const& funcExpr = funcExpression();
	if (auto const* newExpr = dynamic_cast<NewExpression const*>(&funcExpr))
	{
		auto const* contractType = dynamic_cast<ContractType const*>(
			newExpr->typeName().annotation().type);
		if (contractType)
		{
			Logger::instance().error(
				"'new " + contractType->contractDefinition().name() + "()' "
				"(inner contract deployment) is not yet supported. "
				"Requires two-pass compilation to embed child contract bytecode.", m_loc);
		}
	}

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

} // namespace puyasol::builder::sol_ast
