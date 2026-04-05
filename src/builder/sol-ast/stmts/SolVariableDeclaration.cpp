/// @file SolVariableDeclaration.cpp
/// Migrated from VariableDeclarationBuilder.cpp.

#include "builder/sol-ast/stmts/SolVariableDeclaration.h"
#include "builder/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolVariableDeclaration::SolVariableDeclaration(
	StatementContext& _ctx,
	VariableDeclarationStatement const& _node,
	awst::SourceLocation _loc,
	ExpressionBuilder& _exprBuilder)
	: SolStatement(_ctx, std::move(_loc)), m_node(_node), m_exprBuilder(_exprBuilder)
{
}

std::vector<std::shared_ptr<awst::Statement>> SolVariableDeclaration::toAwst()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	auto const& declarations = m_node.declarations();
	auto const* initialValue = m_node.initialValue();

	if (declarations.size() == 1 && declarations[0])
	{
		auto const& decl = *declarations[0];
		auto* type = m_ctx.typeMapper->map(decl.type());

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = m_ctx.makeLoc(decl.location());
		target->wtype = type;
		target->name = decl.name();

		std::shared_ptr<awst::Expression> value;
		if (initialValue)
		{
			// Track function pointer assignments
			if (dynamic_cast<FunctionType const*>(decl.type()))
			{
				if (auto const* initId = dynamic_cast<Identifier const*>(initialValue))
				{
					if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(
							initId->annotation().referencedDeclaration))
						m_exprBuilder.trackFuncPtrTarget(decl.id(), funcDef);
				}
			}

			value = m_ctx.buildExpr(*initialValue);

			// Track constant locals (only if value fits in unsigned long long)
			if (auto const* ratType = dynamic_cast<RationalNumberType const*>(
					initialValue->annotation().type))
			{
				auto val = ratType->literalValue(nullptr);
				if (val > 0 && val <= std::numeric_limits<unsigned long long>::max())
					m_exprBuilder.trackConstantLocal(decl.id(), static_cast<unsigned long long>(val));
			}

			// Upgrade dynamic array to fixed-size when N is known
			if (auto* newArr = dynamic_cast<awst::NewArray*>(value.get()))
			{
				if (!newArr->values.empty())
				{
					if (type && type->kind() == awst::WTypeKind::ReferenceArray)
					{
						auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(type);
						if (refArr && !refArr->arraySize())
						{
							int n = static_cast<int>(newArr->values.size());
							type = m_ctx.typeMapper->createType<awst::ReferenceArray>(
								refArr->elementType(), true, n);
							newArr->wtype = type;
							target->wtype = type;
						}
					}
					// Note: don't upgrade ARC4DynamicArray→ARC4StaticArray here.
					// Subsequent references to the variable use TypeMapper which
					// returns ARC4DynamicArray, causing type mismatches.
				}
			}

			value = builder::TypeCoercion::coerceForAssignment(std::move(value), type, m_loc);
		}
		else
			value = StorageMapper::makeDefaultValue(type, m_loc);

		// Storage pointer alias
		if (decl.referenceLocation() == VariableDeclaration::Location::Storage && initialValue)
		{
			if (dynamic_cast<awst::StateGet const*>(value.get())
				|| dynamic_cast<awst::BoxValueExpression const*>(value.get())
				|| dynamic_cast<awst::AppStateExpression const*>(value.get()))
			{
				auto aliasExpr = value;
				if (auto const* boxVal = dynamic_cast<awst::BoxValueExpression const*>(value.get()))
				{
					auto stateGet = std::make_shared<awst::StateGet>();
					stateGet->sourceLocation = m_loc;
					stateGet->wtype = boxVal->wtype;
					stateGet->field = value;
					stateGet->defaultValue = StorageMapper::makeDefaultValue(boxVal->wtype, m_loc);
					aliasExpr = stateGet;
				}
				else if (auto const* appState = dynamic_cast<awst::AppStateExpression const*>(value.get()))
				{
					auto stateGet = std::make_shared<awst::StateGet>();
					stateGet->sourceLocation = m_loc;
					stateGet->wtype = appState->wtype;
					stateGet->field = value;
					stateGet->defaultValue = StorageMapper::makeDefaultValue(appState->wtype, m_loc);
					aliasExpr = stateGet;
				}
				m_exprBuilder.addStorageAlias(decl.id(), aliasExpr);
				for (auto& p: m_ctx.takePrePending())
					result.push_back(std::move(p));
				for (auto& p: m_ctx.takePending())
					result.push_back(std::move(p));
				return result;
			}
		}

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = m_loc;
		assign->target = std::move(target);
		assign->value = std::move(value);

		for (auto& p: m_ctx.takePrePending())
			result.push_back(std::move(p));
		for (auto& p: m_ctx.takePending())
			result.push_back(std::move(p));
		result.push_back(assign);
	}
	else if (declarations.size() > 1 && initialValue)
	{
		// Tuple destructuring
		auto rhsExpr = m_ctx.buildExpr(*initialValue);
		for (auto& p: m_ctx.takePrePending())
			result.push_back(std::move(p));
		for (auto& p: m_ctx.takePending())
			result.push_back(std::move(p));

		auto singleRhs = std::make_shared<awst::SingleEvaluation>();
		singleRhs->sourceLocation = m_loc;
		singleRhs->wtype = rhsExpr->wtype;
		singleRhs->source = std::move(rhsExpr);
		singleRhs->id = static_cast<int>(m_node.id());
		rhsExpr = std::move(singleRhs);

		for (size_t i = 0; i < declarations.size(); ++i)
		{
			if (!declarations[i]) continue;
			auto const& decl = *declarations[i];
			auto* type = m_ctx.typeMapper->map(decl.type());

			auto target = std::make_shared<awst::VarExpression>();
			target->sourceLocation = m_ctx.makeLoc(decl.location());
			target->wtype = type;
			target->name = decl.name();

			auto itemExpr = std::make_shared<awst::TupleItemExpression>();
			itemExpr->sourceLocation = m_loc;
			itemExpr->wtype = type;
			itemExpr->base = rhsExpr;
			itemExpr->index = static_cast<int>(i);

			auto assign = std::make_shared<awst::AssignmentStatement>();
			assign->sourceLocation = m_loc;
			assign->target = std::move(target);
			assign->value = std::move(itemExpr);
			result.push_back(assign);
		}
	}

	return result;
}

} // namespace puyasol::builder::sol_ast
