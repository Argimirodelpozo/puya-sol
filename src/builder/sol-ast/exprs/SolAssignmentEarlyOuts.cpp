/// @file SolAssignmentEarlyOuts.cpp — pre-buildExpr early-out handlers:
///   tryHandleTransientStateWrite, tryHandleStoragePointerReassign,
///   tryHandleMultiBoxArrayWrite
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageBackend.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/Arc4Defaults.h"

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
		|| !m_ctx.storageBackend
		|| !m_ctx.storageBackend->isTransient(*lhsDecl))
		return std::nullopt;

	auto* sb = m_ctx.storageBackend;
	auto const& name = lhsIdent->name();
	auto* varType = m_ctx.typeMapper.map(lhsDecl->type());
	auto rhs = buildExpr(m_assignment.rightHandSide());

	std::shared_ptr<awst::Expression> newValue;
	if (op == Token::Assign)
	{
		newValue = std::move(rhs);
	}
	else
	{
		auto currentValue = sb->emitReadForVar(*lhsDecl, name, varType, m_loc);
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

	auto stmt = sb->emitWriteForVar(*lhsDecl, name, newValue, m_loc);
	if (stmt)
		m_ctx.pendingStatements.push_back(std::move(stmt));

	// Re-read to yield the written value (Solidity assignment-as-expression).
	return sb->emitReadForVar(*lhsDecl, name, varType, m_loc);
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

	// Mapping-key-param locals hold the box-key prefix as a runtime bytes value.
	// Must do a real bytes write; compile-time alias path (VoidConstant) would lose
	// mutations like `r = a; r[k] = v; r = b; r[k] = v`.
	if (!m_scope.findMappingKeyParam(lhsDecl->id()).empty())
	{
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
			lhsIdent->name(), awst::WType::bytesType(), m_loc);
		return awst::makeAssignmentExpression(std::move(var), std::move(rhsExpr), m_loc);
	}

	auto rhsExpr = buildExpr(m_assignment.rightHandSide());
	auto aliasExpr = rhsExpr;
	if (awst::isRawStorageRead(rhsExpr.get()))
		aliasExpr = StorageMapper::makeStateGetWithDefault(rhsExpr, rhsExpr->wtype, m_loc);
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
		return std::nullopt; // compound assigns on multi-box arrays unsupported

	unsigned elemSize = StorageMapper::arc4StaticArrayElementSize(arrWtype);
	unsigned elemsPerBox = StorageMapper::elementsPerBox(arrWtype);
	auto const* sa = static_cast<awst::ARC4StaticArray const*>(arrWtype);
	auto const* elemArc4Type = sa->elementType();

	// Pin idx to a temp so page and offset can reference it without re-evaluating side effects.
	auto idxExpr = builder::TypeCoercion::checkedIndexToUint64(
		m_ctx.prePendingStatements, buildExpr(*lhsIdx->indexExpression()), m_loc);
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

	// Pin rhs to a temp so we can encode it for box_replace and also return it as the result.
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
		valueBytes = awst::makeAsBytes(std::move(encode), m_loc);
	}
	else
	{
		valueBytes = awst::makeAsBytes(std::move(valForEncode), m_loc);
	}

	// box_replace(boxKey, offset, valueBytes)
	auto replace = awst::makeIntrinsicCall(
		"box_replace", awst::WType::voidType(), m_loc);
	replace->stackArgs.push_back(std::move(boxKey));
	replace->stackArgs.push_back(std::move(offsetExpr));
	replace->stackArgs.push_back(std::move(valueBytes));
	m_ctx.pendingStatements.push_back(
		awst::makeExpressionStatement(std::move(replace), m_loc));

	return awst::makeVarExpression(valVarName, valVar->wtype, m_loc); // assignment-as-expression
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleBoxedArrayElemWrite()
{
	using namespace solidity::frontend;
	if (m_assignment.assignmentOperator() != Token::Assign)
		return std::nullopt;

	// Match `a[i].field = v` (struct-element field write) or `a[i] = v` (whole element).
	Expression const* cursor = &m_assignment.leftHandSide();
	std::string fieldName;
	if (auto const* ma = dynamic_cast<MemberAccess const*>(cursor))
	{
		fieldName = ma->memberName();
		cursor = &ma->expression();
	}
	auto const* ia = dynamic_cast<IndexAccess const*>(cursor);
	if (!ia || !ia->indexExpression())
		return std::nullopt;
	auto const* arrType = dynamic_cast<ArrayType const*>(
		ia->baseExpression().annotation().type);
	if (!arrType || !arrType->isDynamicallySized() || arrType->isByteArrayOrString())
		return std::nullopt;

	// Only a box-keyed array REF PARAM (handle model); state-var arrays keep the COW path.
	auto const* baseId = dynamic_cast<Identifier const*>(&ia->baseExpression());
	if (!baseId) return std::nullopt;
	auto const* vd = dynamic_cast<VariableDeclaration const*>(
		baseId->annotation().referencedDeclaration);
	if (!vd) return std::nullopt;
	std::string keyParam = m_scope.findMappingKeyParam(vd->id());
	if (keyParam.empty())
		return std::nullopt;

	// Fixed-size struct element → constant offset.
	auto const* st = dynamic_cast<StructType const*>(arrType->baseType());
	if (!st) return std::nullopt;
	auto* elemArc4 = m_ctx.typeMapper.mapSolTypeToARC4(arrType->baseType());
	int elemSize = builder::computeEncodedElementSize(elemArc4);
	if (elemSize <= 0) return std::nullopt;

	// Field offset within the element (Σ preceding ARC4 sizes); whole element if no field.
	uint64_t fieldOff = 0;
	awst::WType const* slotArc4 = elemArc4;
	Type const* valSol = arrType->baseType();
	if (!fieldName.empty())
	{
		bool found = false;
		for (auto const& m: st->structDefinition().members())
		{
			if (m->name() == fieldName)
			{
				slotArc4 = m_ctx.typeMapper.mapSolTypeToARC4(m->type());
				valSol = m->type();
				found = true;
				break;
			}
			fieldOff += static_cast<uint64_t>(builder::computeEncodedElementSize(
				m_ctx.typeMapper.mapSolTypeToARC4(m->type())));
		}
		if (!found) return std::nullopt;
	}

	// box key = the runtime bytes the caller passed; offset = 2 (uint16 len prefix) + i*elemSize + fieldOff.
	auto boxKey = awst::makeReinterpretCast(
		awst::makeVarExpression(keyParam, awst::WType::bytesType(), m_loc),
		awst::WType::boxKeyType(), m_loc);
	auto idx = builder::TypeCoercion::checkedIndexToUint64(
		m_ctx.prePendingStatements, buildExpr(*ia->indexExpression()), m_loc);
	auto offset = awst::makeUInt64BinOp(
		awst::makeUInt64BinOp(std::move(idx), awst::UInt64BinaryOperator::Mult,
			awst::makeIntegerConstant(static_cast<uint64_t>(elemSize), m_loc), m_loc),
		awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant(static_cast<uint64_t>(2 + fieldOff), m_loc), m_loc);

	// rhs → ARC4 bytes of the slot type; pin to a temp so it's also the assignment result.
	auto rhs = builder::TypeCoercion::coerceForAssignment(
		buildExpr(m_assignment.rightHandSide()), m_ctx.typeMapper.map(valSol), m_loc);
	static int s_baeCtr = 0;
	std::string vn = "__bae_val_" + std::to_string(s_baeCtr++);
	auto vv = awst::makeVarExpression(vn, rhs->wtype, m_loc);
	m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(vv, std::move(rhs), m_loc));
	auto valForEnc = awst::makeVarExpression(vn, vv->wtype, m_loc);
	std::shared_ptr<awst::Expression> valBytes =
		(valForEnc->wtype != slotArc4 && valForEnc->wtype->name() != slotArc4->name())
			? awst::makeAsBytes(awst::makeARC4Encode(
				std::move(valForEnc), const_cast<awst::WType*>(slotArc4), m_loc), m_loc)
			: awst::makeAsBytes(std::move(valForEnc), m_loc);

	m_ctx.pendingStatements.push_back(awst::makeExpressionStatement(
		awst::makeBoxReplace(std::move(boxKey), std::move(offset), std::move(valBytes), m_loc),
		m_loc));
	return awst::makeVarExpression(vn, vv->wtype, m_loc);
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleOffsetStructRefFieldWrite()
{
	// `s.field = v` where `s` is a struct storage-ref PARAM carrying a runtime OFFSET (handle-model
	// dual handle): write the field slice directly via box_replace(key, offsetVar+fieldOff). For a
	// whole-box caller the offset is 0 — byte-identical to a direct field write at the box head.
	auto const* member = dynamic_cast<MemberAccess const*>(&m_assignment.leftHandSide());
	if (!member) return std::nullopt;
	auto const* baseId = dynamic_cast<Identifier const*>(&member->expression());
	if (!baseId) return std::nullopt;
	auto const* vd = dynamic_cast<VariableDeclaration const*>(
		baseId->annotation().referencedDeclaration);
	if (!vd) return std::nullopt;
	std::string offVar = m_scope.findStructRefOffset(vd->id());
	std::string keyParam = m_scope.findMappingKeyParam(vd->id());
	if (offVar.empty() || keyParam.empty()) return std::nullopt;
	if (m_assignment.assignmentOperator() != Token::Assign) return std::nullopt; // compound: fall back

	auto const* st = dynamic_cast<StructType const*>(vd->type());
	if (!st) return std::nullopt;
	// Dynamic-layout structs would have offset-dependent field positions; leave to the generic path.
	if (builder::computeEncodedElementSize(m_ctx.typeMapper.mapSolTypeToARC4(vd->type())) <= 0)
		return std::nullopt;

	std::string fieldName = member->memberName();
	uint64_t fieldOff = 0;
	awst::WType const* slotArc4 = nullptr;
	Type const* valSol = nullptr;
	for (auto const& m: st->structDefinition().members())
	{
		if (m->name() == fieldName)
		{
			slotArc4 = m_ctx.typeMapper.mapSolTypeToARC4(m->type());
			valSol = m->type();
			break;
		}
		fieldOff += static_cast<uint64_t>(builder::computeEncodedElementSize(
			m_ctx.typeMapper.mapSolTypeToARC4(m->type())));
	}
	if (!slotArc4 || !valSol) return std::nullopt;

	// box key = s's runtime bytes; total offset = offsetVar + fieldOff.
	auto boxKey = awst::makeReinterpretCast(
		awst::makeVarExpression(keyParam, awst::WType::bytesType(), m_loc),
		awst::WType::boxKeyType(), m_loc);
	auto offset = awst::makeUInt64BinOp(
		awst::makeVarExpression(offVar, awst::WType::uint64Type(), m_loc),
		awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant(fieldOff, m_loc), m_loc);

	auto rhs = builder::TypeCoercion::coerceForAssignment(
		buildExpr(m_assignment.rightHandSide()), m_ctx.typeMapper.map(valSol), m_loc);
	static int s_osCtr = 0;
	std::string vn = "__osref_val_" + std::to_string(s_osCtr++);
	auto vv = awst::makeVarExpression(vn, rhs->wtype, m_loc);
	m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(vv, std::move(rhs), m_loc));
	auto valForEnc = awst::makeVarExpression(vn, vv->wtype, m_loc);
	std::shared_ptr<awst::Expression> valBytes =
		(valForEnc->wtype != slotArc4 && valForEnc->wtype->name() != slotArc4->name())
			? awst::makeAsBytes(awst::makeARC4Encode(
				std::move(valForEnc), const_cast<awst::WType*>(slotArc4), m_loc), m_loc)
			: awst::makeAsBytes(std::move(valForEnc), m_loc);

	m_ctx.pendingStatements.push_back(awst::makeExpressionStatement(
		awst::makeBoxReplace(std::move(boxKey), std::move(offset), std::move(valBytes), m_loc),
		m_loc));
	return awst::makeVarExpression(vn, vv->wtype, m_loc);
}

} // namespace puyasol::builder::sol_ast
