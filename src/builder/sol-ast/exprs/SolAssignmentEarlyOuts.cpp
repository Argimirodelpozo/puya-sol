/// @file SolAssignmentEarlyOuts.cpp
/// LHS-shape early-out handlers extracted from SolAssignment.cpp. Each
/// returns the assignment-as-expression result if it claims ownership of
/// the shape, or std::nullopt to fall through to the generic dispatch in
/// SolAssignment::toAwst:
///   - tryHandleTransientStateWrite: `tx = v` for transient state vars
///   - tryHandleStoragePointerReassign: `storagePtr = otherStorage`
///   - tryHandleMultiBoxArrayWrite: multi-box-paged array element assign
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

std::optional<std::shared_ptr<awst::Expression>> SolAssignment::tryHandleTransientStateWrite()
{
	Token op = m_assignment.assignmentOperator();
	auto const* lhsIdent = dynamic_cast<Identifier const*>(&m_assignment.leftHandSide());
	if (!lhsIdent) return std::nullopt;
	auto const* lhsDecl = dynamic_cast<VariableDeclaration const*>(
		lhsIdent->annotation().referencedDeclaration);
	if (!lhsDecl
		|| !lhsDecl->isStateVariable()
		|| lhsDecl->referenceLocation() != VariableDeclaration::Location::Transient
		|| !m_ctx.transientStorage
		|| !m_ctx.transientStorage->isTransient(*lhsDecl))
		return std::nullopt;

	auto* ts = m_ctx.transientStorage;
	auto* varType = m_ctx.typeMapper.map(lhsDecl->type());
	auto rhs = buildExpr(m_assignment.rightHandSide());

	std::shared_ptr<awst::Expression> newValue;
	if (op == Token::Assign)
	{
		newValue = std::move(rhs);
	}
	else
	{
		auto currentValue = ts->buildRead(lhsIdent->name(), varType, m_loc);
		auto* solType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, op, solType, currentValue, rhs, m_loc);
		if (builderResult)
			newValue = std::move(builderResult);
		else
			newValue = m_ctx.buildBinaryOp(
				op, std::move(currentValue), std::move(rhs), varType, m_loc);
	}

	newValue = builder::TypeCoercion::coerceForAssignment(std::move(newValue), varType, m_loc);

	auto stmt = ts->buildWrite(lhsIdent->name(), newValue, m_loc);
	if (stmt)
		m_ctx.pendingStatements.push_back(std::move(stmt));

	// Return the new value so assignment-as-expression yields the
	// written value (Solidity semantics).
	return ts->buildRead(lhsIdent->name(), varType, m_loc);
}

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

	// Mapping-key-param locals (`mapping(K=>V) storage r` returned from a
	// function or declared inside one) hold the box-key prefix as a runtime
	// bytes value. Reassigning them must do an actual bytes write; the
	// compile-time alias path drops the side effect (returns VoidConstant)
	// which is fine for state-var aliases but loses runtime mutations like
	// `r = a; r[k] = v; r = b; r[k] = v;`.
	if (!m_scope.findMappingKeyParam(lhsDecl->id()).empty())
	{
		auto rhsExpr = buildExpr(m_assignment.rightHandSide());
		if (rhsExpr->wtype != awst::WType::bytesType())
		{
			rhsExpr = builder::TypeCoercion::coerceForAssignment(
				std::move(rhsExpr), awst::WType::bytesType(), m_loc);
		}
		auto var = awst::makeVarExpression(
			lhsIdent->name(), awst::WType::bytesType(), m_loc);
		return awst::makeAssignmentExpression(std::move(var), std::move(rhsExpr), m_loc);
	}

	auto rhsExpr = buildExpr(m_assignment.rightHandSide());
	auto aliasExpr = rhsExpr;
	if (dynamic_cast<awst::BoxValueExpression const*>(rhsExpr.get())
		|| dynamic_cast<awst::AppStateExpression const*>(rhsExpr.get()))
	{
		auto sg = awst::makeStateGet(rhsExpr, StorageMapper::makeDefaultValue(rhsExpr->wtype, m_loc), rhsExpr->wtype, m_loc);
		aliasExpr = sg;
	}
	m_scope.setStorageAlias(
		lhsDecl->id(), StorageAlias::stateRead(std::move(aliasExpr)));
	auto voidExpr = awst::makeVoidConstant(m_loc);
	return std::shared_ptr<awst::Expression>(voidExpr);
}

std::optional<std::shared_ptr<awst::Expression>> SolAssignment::tryHandleMultiBoxArrayWrite()
{
	auto const* lhsIdx = dynamic_cast<IndexAccess const*>(&m_assignment.leftHandSide());
	if (!lhsIdx) return std::nullopt;
	auto const* ident = dynamic_cast<Identifier const*>(&lhsIdx->baseExpression());
	if (!ident) return std::nullopt;
	auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
		ident->annotation().referencedDeclaration);
	if (!varDecl || !varDecl->isStateVariable()
		|| varDecl->isConstant() || varDecl->immutable())
		return std::nullopt;
	auto const* arrWtype = m_ctx.typeMapper.map(varDecl->type());
	if (!builder::StorageMapper::isMultiBoxArray(arrWtype))
		return std::nullopt;
	if (!lhsIdx->indexExpression()) return std::nullopt;

	Token op = m_assignment.assignmentOperator();
	if (op != Token::Assign)
	{
		// Compound assignments (`arr[i] += v`) on multi-box arrays not
		// yet supported. Falls through to default handler which will fail.
		return std::nullopt;
	}

	unsigned elemSize = StorageMapper::arc4StaticArrayElementSize(arrWtype);
	unsigned elemsPerBox = StorageMapper::elementsPerBox(arrWtype);
	auto const* sa = static_cast<awst::ARC4StaticArray const*>(arrWtype);
	auto const* elemArc4Type = sa->elementType();

	// Pin idx to a temp so we can reference it for both page and offset
	// without re-evaluating any side effects.
	auto idxExpr = buildExpr(*lhsIdx->indexExpression());
	if (idxExpr->wtype != awst::WType::uint64Type())
		idxExpr = builder::TypeCoercion::implicitNumericCast(
			std::move(idxExpr), awst::WType::uint64Type(), m_loc);
	static int s_mbWCounter = 0;
	std::string idxVarName = "__mb_widx_" + std::to_string(s_mbWCounter++);
	auto idxVar = awst::makeVarExpression(idxVarName, awst::WType::uint64Type(), m_loc);
	m_ctx.prePendingStatements.push_back(
		awst::makeAssignmentStatement(idxVar, std::move(idxExpr), m_loc));

	// page = idx / elemsPerBox
	auto pageExpr = awst::makeUInt64BinOp(
		awst::makeVarExpression(idxVarName, awst::WType::uint64Type(), m_loc),
		awst::UInt64BinaryOperator::FloorDiv,
		awst::makeIntegerConstant(elemsPerBox, m_loc), m_loc);

	// offset = (idx % elemsPerBox) * elemSize
	auto remExpr = awst::makeUInt64BinOp(
		awst::makeVarExpression(idxVarName, awst::WType::uint64Type(), m_loc),
		awst::UInt64BinaryOperator::Mod,
		awst::makeIntegerConstant(elemsPerBox, m_loc), m_loc);
	auto offsetExpr = awst::makeUInt64BinOp(
		std::move(remExpr), awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant(elemSize, m_loc), m_loc);

	// boxKey = bytes(varName) ++ itob(page)
	auto nameBytes = awst::makeUtf8BytesConstant(
		varDecl->name(), m_loc, awst::WType::boxKeyType());
	auto boxKey = awst::makeConcat(
		std::move(nameBytes), awst::makeItob(std::move(pageExpr), m_loc), m_loc);
	boxKey->wtype = awst::WType::boxKeyType();

	// rhs → ARC4-encoded element bytes.
	auto rhs = buildExpr(m_assignment.rightHandSide());
	auto* expectedNative = m_ctx.typeMapper.map(
		m_assignment.rightHandSide().annotation().type);
	rhs = builder::TypeCoercion::coerceForAssignment(
		std::move(rhs), expectedNative, m_loc);

	// Pin rhs to a temp so we can both encode it for box_replace and return
	// the raw value as the assignment-as-expression result.
	static int s_mbVCounter = 0;
	std::string valVarName = "__mb_val_" + std::to_string(s_mbVCounter++);
	auto valVar = awst::makeVarExpression(valVarName, rhs->wtype, m_loc);
	m_ctx.prePendingStatements.push_back(
		awst::makeAssignmentStatement(valVar, std::move(rhs), m_loc));

	auto valForEncode = awst::makeVarExpression(valVarName, valVar->wtype, m_loc);
	std::shared_ptr<awst::Expression> valueBytes;
	bool valueIsNative = valForEncode->wtype != elemArc4Type
		&& valForEncode->wtype->name() != elemArc4Type->name();
	if (valueIsNative)
	{
		auto encode = awst::makeARC4Encode(
			std::move(valForEncode),
			const_cast<awst::WType*>(elemArc4Type), m_loc);
		valueBytes = awst::makeReinterpretCast(
			std::move(encode), awst::WType::bytesType(), m_loc);
	}
	else
	{
		valueBytes = awst::makeReinterpretCast(
			std::move(valForEncode), awst::WType::bytesType(), m_loc);
	}

	// box_replace(boxKey, offset, valueBytes)
	auto replace = awst::makeIntrinsicCall(
		"box_replace", awst::WType::voidType(), m_loc);
	replace->stackArgs.push_back(std::move(boxKey));
	replace->stackArgs.push_back(std::move(offsetExpr));
	replace->stackArgs.push_back(std::move(valueBytes));
	m_ctx.pendingStatements.push_back(
		awst::makeExpressionStatement(std::move(replace), m_loc));

	// Return the assigned value (assignment-as-expression).
	return awst::makeVarExpression(valVarName, valVar->wtype, m_loc);
}

} // namespace puyasol::builder::sol_ast
