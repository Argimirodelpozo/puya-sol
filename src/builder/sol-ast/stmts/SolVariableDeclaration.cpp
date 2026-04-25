/// @file SolVariableDeclaration.cpp
/// Migrated from VariableDeclarationBuilder.cpp.

#include "builder/sol-ast/stmts/SolVariableDeclaration.h"
#include "builder/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/assembly/AssemblyBuilder.h"
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

		auto target = awst::makeVarExpression(m_exprBuilder.resolveVarName(decl.name(), decl.id()), type, m_ctx.makeLoc(decl.location()));

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

			// Slot-based storage reference: initialized from internal function call
			// that returns a storage reference (typically has .slot := in assembly).
			// Register as slot ref so IndexAccess translates to sload/sstore.
			if (dynamic_cast<awst::SubroutineCallExpression const*>(value.get()))
			{
				m_exprBuilder.addSlotStorageRef(decl.id(), value);
				// Also emit the call as an assignment so the slot value is available
				auto slotVar = awst::makeVarExpression(decl.name(), awst::WType::biguintType(), m_loc);

				auto assign = awst::makeAssignmentStatement(std::move(slotVar), std::move(value), m_loc);
				result.push_back(std::move(assign));

				for (auto& p: m_ctx.takePrePending())
					result.push_back(std::move(p));
				for (auto& p: m_ctx.takePending())
					result.push_back(std::move(p));
				return result;
			}
		}

		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(value), m_loc);

		for (auto& p: m_ctx.takePrePending())
			result.push_back(std::move(p));
		for (auto& p: m_ctx.takePending())
			result.push_back(std::move(p));

		// EVM free-memory-pointer simulation: `T memory t;` (no initializer)
		// allocates fresh memory and bumps mload(0x40) by sizeof(T). We mirror
		// this so contracts that read mload(0x40) see the expected advance.
		// Memory locals with initializers are pointer copies in EVM (no alloc).
		if (!initialValue
			&& decl.referenceLocation() == VariableDeclaration::Location::Memory)
		{
			int sz = builder::TypeCoercion::computeEncodedElementSize(type);
			if (sz > 0)
				for (auto& s: builder::AssemblyBuilder::emitFreeMemoryBump(
						sz, m_loc, static_cast<int>(decl.id())))
					result.push_back(std::move(s));
		}

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

			auto target = awst::makeVarExpression(decl.name(), type, m_ctx.makeLoc(decl.location()));

			auto itemExpr = std::make_shared<awst::TupleItemExpression>();
			itemExpr->sourceLocation = m_loc;
			itemExpr->wtype = type;
			itemExpr->base = rhsExpr;
			itemExpr->index = static_cast<int>(i);

			auto assign = awst::makeAssignmentStatement(std::move(target), std::move(itemExpr), m_loc);
			result.push_back(assign);
		}
	}

	return result;
}

} // namespace puyasol::builder::sol_ast
