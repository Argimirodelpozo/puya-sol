/// @file Storage-pointer rebinding and assignment value conversion.
#include "builder/ast/exprs/SolAssignment.h"
#include "builder/eb/MappingPrefix.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

std::optional<std::shared_ptr<awst::Expression>> SolAssignment::tryHandleStoragePointerReassign()
{
	Token op = m_assignment.assignmentOperator();
	if (op != Token::Assign) return std::nullopt;
	auto const* lhsIdent = dynamic_cast<Identifier const*>(&m_assignment.leftHandSide());
	if (!lhsIdent) return std::nullopt;
	auto const* lhsDecl = dynamic_cast<VariableDeclaration const*>(
		lhsIdent->annotation().referencedDeclaration);
	if (!lhsDecl
		|| lhsDecl->referenceLocation() != VariableDeclaration::Location::Storage
		|| lhsDecl->isStateVariable())
		return std::nullopt;

	// Mapping-key-param locals hold the box-key prefix as a runtime bytes value.
	// Must do a real bytes write; compile-time alias path (VoidConstant) would lose
	// mutations like `r = a; r[k] = v; r = b; r[k] = v`.
	if (auto const& keyParam = m_scope.bindings.mappingKeyParams.get(lhsDecl->id()); !keyParam.empty())
	{
		if (!m_ctx.typeMapper.profile().evmStorageLayout && containsMappingType(lhsDecl->type()))
			return awst::makeAssignmentExpression(
				awst::makeVarExpression(keyParam, awst::WType::bytesType(), m_loc),
				storageReferenceKey(m_ctx, m_scope, m_assignment.rightHandSide(), m_loc), m_loc);
		auto rhsExpr = buildExpr(m_assignment.rightHandSide());
		// RHS storage-ref element read (`self[k]` → StateGet(BoxValueExpression)):
		// lift the box KEY, not decoded value (mirrors SolInternalCall::extractMappingKeyPrefix).
		// V4 shape: `position = self[positionKey]` in Position.get.
		auto keyExpr = awst::unwrapStateGet(rhsExpr);
		if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(keyExpr.get()))
			rhsExpr = awst::makeReinterpretCast(box->key, awst::WType::bytesType(), m_loc);
		else if (rhsExpr->wtype != awst::WType::bytesType())
		{
			rhsExpr = builder::TypeCoercion::coerceForAssignment(
				std::move(rhsExpr), awst::WType::bytesType(), m_loc);
		}
		auto var = awst::makeVarExpression(
			keyParam, awst::WType::bytesType(), m_loc);
		return awst::makeAssignmentExpression(std::move(var), std::move(rhsExpr), m_loc);
	}

	// The rebind below is COMPILE-TIME-ONLY (flat alias map, no runtime
	// artifact): inside a conditionally-executed region it would apply
	// unconditionally to every later use (`if (c) p = a2; p.push(1);`
	// always pushed to a2). No sound lowering exists yet — fail loud.
	if (m_ctx.conditionalDepth > 0)
		Logger::instance().error(
			"storage-pointer reassignment inside a conditionally-executed "
			"block (if/else branch, loop body, ternary or short-circuit arm) "
			"is not supported: the rebind is resolved at compile time and "
			"would apply unconditionally to all following uses. Hoist the "
			"reassignment, or select at initialization "
			"(`T storage p = cond ? a : b;`).", m_loc);
	auto rhsExpr = buildExpr(m_assignment.rightHandSide());
	if (!m_ctx.typeMapper.profile().evmStorageLayout && containsMappingType(lhsDecl->type()))
	{
		auto holder = resolveBuiltStorageHolder(m_ctx, rhsExpr, m_loc);
		if (!holder.key) throw SizeError("aggregate alias requires a resolved storage holder");
		rhsExpr = std::move(holder.value);
	}
	auto aliasExpr = rhsExpr;
	if (awst::isRawStorageRead(rhsExpr.get()))
		aliasExpr = StorageMapper::makeStateGetWithDefault(rhsExpr, rhsExpr->wtype, m_loc);
	m_scope.bindings.storageAliases.set(
		lhsDecl->id(), StorageAlias::stateRead(aliasExpr));
	return aliasExpr;
}

std::shared_ptr<awst::Expression> SolAssignment::computeAggregateStoreValue(
	Token _op,
	std::shared_ptr<awst::Expression> _current,
	std::shared_ptr<awst::Expression> _rhs,
	awst::WType const* _nativeW)
{
	if (_op != Token::Assign)
	{
		if (!awst::structurallyEquivalent(_current->wtype, _nativeW))
			_current = awst::makeARC4Decode(
				std::move(_current), _nativeW, m_loc);
		_rhs = widenSignedCompoundRhs(std::move(_rhs));
		_rhs = eb::AssignmentHelper::computeCompoundOrFallback(
			m_ctx, _op, _op, m_assignment.leftHandSide().annotation().type,
			std::move(_current), std::move(_rhs), _nativeW, m_loc);
	}
	if (_op == Token::Assign)
		return builder::ConversionPlan{m_assignment.rightHandSide().annotation().type,
			m_assignment.leftHandSide().annotation().type, _nativeW,
			builder::ConversionPlan::Context::Assignment}.emit(
				std::move(_rhs), m_loc, &m_ctx.preEffects());
	return builder::TypeCoercion::coerceForAssignment(
		std::move(_rhs), _nativeW, m_loc, &m_ctx.preEffects());
}

} // namespace puyasol::builder::sol_ast
