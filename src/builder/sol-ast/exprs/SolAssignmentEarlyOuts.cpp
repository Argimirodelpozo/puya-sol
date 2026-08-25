/// @file SolAssignmentEarlyOuts.cpp — pre-buildExpr early-out handlers:
///   tryHandleTransientStateWrite, tryHandleStoragePointerReassign,
///   tryHandleMultiBoxArrayWrite
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "awst/NameGen.h"
#include "builder/sol-ast/EffectScan.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageBackend.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

#include <algorithm>

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
		rhs = widenSignedCompoundRhs(std::move(rhs));
		newValue = eb::AssignmentHelper::computeCompoundOrFallback(
			m_ctx, op, op, solType, std::move(currentValue),
			std::move(rhs), varType, m_loc);
	}

	newValue = builder::TypeCoercion::coerceForAssignment(std::move(newValue), varType, m_loc);
	// Eval-once so the same value feeds BOTH the write and the returned
	// assignment-expression value (the tree is shared by two parents).
	newValue = awst::makeEvalOnce(std::move(newValue), m_loc);

	auto stmt = sb->emitWriteForVar(*lhsDecl, name, newValue, m_loc);
	if (stmt)
		m_ctx.postEffects().push_back(std::move(stmt));

	// Yield the ASSIGNED value directly, not a storage re-read: the write is
	// queued POST-pending, so `uint a = (t = 5)` re-read t BEFORE the write and
	// got the stale value. The assignment expression's value in Solidity is the
	// assigned value regardless.
	return newValue;
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
	Token const op = m_assignment.assignmentOperator();

	// Peel an arbitrary member/index path to the multi-box state variable. The
	// first index selects one element-aligned page slice; any remaining path is
	// replayed over that element and the complete element is written back once.
	std::vector<Expression const*> path;
	Expression const* cursor = &m_assignment.leftHandSide();
	for (;;)
	{
		if (auto const* index = dynamic_cast<IndexAccess const*>(cursor))
		{
			if (!index->indexExpression())
				return std::nullopt;
			path.push_back(cursor);
			cursor = &index->baseExpression();
			continue;
		}
		if (auto const* member = dynamic_cast<MemberAccess const*>(cursor))
		{
			path.push_back(cursor);
			cursor = &member->expression();
			continue;
		}
		break;
	}
	auto const* ident = dynamic_cast<Identifier const*>(cursor);
	if (!ident || path.empty()) return std::nullopt;
	auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
		ident->annotation().referencedDeclaration);
	if (!varDecl || !varDecl->isStateVariable()
		|| varDecl->isConstant() || varDecl->immutable())
		return std::nullopt;
	auto const* arrWtype = m_ctx.typeMapper.map(varDecl->type());
	if (!builder::StorageMapper::isMultiBoxArray(arrWtype))
		return std::nullopt;
	std::reverse(path.begin(), path.end());
	auto const* rootIndex = dynamic_cast<IndexAccess const*>(path.front());
	if (!rootIndex)
		return std::nullopt;

	unsigned elemSize = StorageMapper::arc4StaticArrayElementSize(arrWtype);
	unsigned elemsPerBox = StorageMapper::elementsPerBox(arrWtype);
	auto const* sa = static_cast<awst::ARC4StaticArray const*>(arrWtype);
	auto const* elemArc4Type = sa->elementType();
	if (elemSize == 0 || elemsPerBox == 0)
		return std::nullopt;
	if (path.size() > 1
		&& elemSize > static_cast<unsigned>(StorageMapper::kAvmStackValueMax))
	{
		Logger::instance().error(
			"nested write into a multi-box array element larger than the AVM "
			"stack-value limit is not representable as one recursive value", m_loc);
		return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc)};
	}

	// Pin idx to a temp so page and offset can reference it without re-evaluating side effects.
	auto idxExpr = builder::TypeCoercion::checkedIndexToUint64(
		m_ctx.preEffects(), buildExpr(*rootIndex->indexExpression()), m_loc);
	std::string idxVarName = "__mb_widx_" + std::to_string(awst::NameGen::next("SolAssignmentEarlyOuts.s_mbWCounter"));
	m_ctx.preEffects().push_back(
		awst::makeAssignmentStatement(
			awst::makeVarExpression(idxVarName, awst::WType::uint64Type(), m_loc),
			std::move(idxExpr), m_loc));
	auto idxVar = [&]() {
		return awst::makeVarExpression(
			idxVarName, awst::WType::uint64Type(), m_loc);
	};
	m_ctx.preEffects().push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeNumericCompare(idxVar(), awst::NumericComparison::Lt,
				awst::makeIntegerConstant(
					static_cast<uint64_t>(sa->arraySize()), m_loc), m_loc),
			m_loc, "array index out of bounds"), m_loc));

	auto makeBoxKey = [&]() {
		auto page = awst::makeUInt64BinOp(
			idxVar(), awst::UInt64BinaryOperator::FloorDiv,
			awst::makeIntegerConstant(elemsPerBox, m_loc), m_loc);
		auto key = awst::makeConcat(
			awst::makeUtf8BytesConstant(
				m_ctx.storageMapper.physicalBindingFor(*varDecl).name,
				m_loc, awst::WType::boxKeyType()),
			awst::makeItob(std::move(page), m_loc), m_loc);
		key->wtype = awst::WType::boxKeyType();
		return key;
	};
	auto makeOffset = [&]() {
		return awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(idxVar(), awst::UInt64BinaryOperator::Mod,
				awst::makeIntegerConstant(elemsPerBox, m_loc), m_loc),
			awst::UInt64BinaryOperator::Mult,
			awst::makeIntegerConstant(elemSize, m_loc), m_loc);
	};

	std::shared_ptr<awst::Expression> target;
	std::string elemName;
	if (path.size() == 1)
		target = awst::makeVarExpression("__mb_direct", elemArc4Type, m_loc);
	else
	{
		elemName = "__mb_root_" + std::to_string(
			awst::NameGen::next("SolAssignmentEarlyOuts.multiBoxRoot"));
		auto bytes = awst::makeBoxExtract(
			makeBoxKey(), makeOffset(),
			awst::makeIntegerConstant(elemSize, m_loc), m_loc);
		m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(elemName, elemArc4Type, m_loc),
			awst::makeReinterpretCast(
				std::move(bytes), elemArc4Type, m_loc), m_loc));
		target = awst::makeVarExpression(elemName, elemArc4Type, m_loc);

		for (size_t i = 1; i < path.size(); ++i)
		{
			if (auto const* index = dynamic_cast<IndexAccess const*>(path[i]))
			{
				auto nestedIndex = builder::TypeCoercion::checkedIndexToUint64(
					m_ctx.preEffects(), buildExpr(*index->indexExpression()), m_loc);
				awst::WType const* nestedElem = nullptr;
				if (auto const* array = dynamic_cast<awst::ARC4DynamicArray const*>(target->wtype))
					nestedElem = array->elementType();
				else if (auto const* array = dynamic_cast<awst::ARC4StaticArray const*>(target->wtype))
					nestedElem = array->elementType();
				else if (auto const* array = dynamic_cast<awst::ReferenceArray const*>(target->wtype))
					nestedElem = array->elementType();
				if (!nestedElem)
					return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc)};
				target = awst::makeIndexExpression(
					std::move(target), std::move(nestedIndex), nestedElem, m_loc);
				continue;
			}
			auto const* member = dynamic_cast<MemberAccess const*>(path[i]);
			auto const* structure = member
				? dynamic_cast<awst::ARC4Struct const*>(target->wtype) : nullptr;
			awst::WType const* fieldType = nullptr;
			if (structure)
				for (auto const& [name, type]: structure->fields())
					if (name == member->memberName()) { fieldType = type; break; }
			if (!member || !fieldType)
				return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc)};
			target = awst::makeFieldExpression(
				std::move(target), member->memberName(), fieldType, m_loc);
		}
	}

	auto rhs = buildExpr(m_assignment.rightHandSide());
	auto* expectedNative = m_ctx.typeMapper.map(
		m_assignment.leftHandSide().annotation().type);
	if (op != Token::Assign)
	{
		std::shared_ptr<awst::Expression> current = target;
		// Direct element (`path.size()==1`): `target` is the write-shape
		// PLACEHOLDER `__mb_direct` — never assigned. The compound read must
		// fetch the element from its page like the nested branch does;
		// decoding the placeholder read an uninitialized var (uint64 0) and
		// `big[i] += d` died on `b< wanted bigint but got uint64`.
		if (path.size() == 1)
			current = awst::makeReinterpretCast(
				awst::makeBoxExtract(makeBoxKey(), makeOffset(),
					awst::makeIntegerConstant(elemSize, m_loc), m_loc),
				elemArc4Type, m_loc);
		if (!awst::structurallyEquivalent(current->wtype, expectedNative))
			current = awst::makeARC4Decode(
				std::move(current), expectedNative, m_loc);
		rhs = widenSignedCompoundRhs(std::move(rhs));
		rhs = eb::AssignmentHelper::computeCompoundOrFallback(
			m_ctx, op, op, m_assignment.leftHandSide().annotation().type,
			std::move(current), std::move(rhs), expectedNative, m_loc);
	}
	rhs = builder::TypeCoercion::coerceForAssignment(
		std::move(rhs), expectedNative, m_loc);

	// Pin rhs to a temp so we can encode it for box_replace and also return it as the result.
	std::string valVarName = "__mb_val_" + std::to_string(awst::NameGen::next("SolAssignmentEarlyOuts.s_mbVCounter"));
	auto valVar = awst::makeVarExpression(valVarName, rhs->wtype, m_loc);
	m_ctx.preEffects().push_back(
		awst::makeAssignmentStatement(valVar, std::move(rhs), m_loc));

	std::shared_ptr<awst::Expression> stored =
		awst::makeVarExpression(valVarName, valVar->wtype, m_loc);
	if (!awst::structurallyEquivalent(stored->wtype, target->wtype))
		stored = awst::makeARC4Encode(std::move(stored), target->wtype, m_loc);

	std::shared_ptr<awst::Expression> valueBytes;
	if (path.size() == 1)
		valueBytes = awst::makeAsBytes(std::move(stored), m_loc);
	else
	{
		m_ctx.postEffects().push_back(awst::makeAssignmentStatement(
			std::move(target), std::move(stored), m_loc));
		valueBytes = awst::makeAsBytes(
			awst::makeVarExpression(elemName, elemArc4Type, m_loc), m_loc);
	}

	// box_replace(boxKey, offset, valueBytes)
	auto replace = awst::makeIntrinsicCall(
		"box_replace", awst::WType::voidType(), m_loc);
	replace->stackArgs.push_back(makeBoxKey());
	replace->stackArgs.push_back(makeOffset());
	replace->stackArgs.push_back(std::move(valueBytes));
	m_ctx.postEffects().push_back(
		awst::makeExpressionStatement(std::move(replace), m_loc));

	return awst::makeVarExpression(valVarName, valVar->wtype, m_loc); // assignment-as-expression
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleBoxedAggregatePathWrite()
{
	using namespace solidity::frontend;
	Token const op = m_assignment.assignmentOperator();

	// Collect the complete lvalue path, not a prescribed `a[i].field` shape.
	// Replaying it over a temporary root value gives one COW algorithm for any
	// mixture of array indices and nested struct members, regardless of whether
	// the root aggregate is itself an array or a struct.
	std::vector<Expression const*> path;
	Expression const* cursor = &m_assignment.leftHandSide();
	for (;;)
	{
		if (auto const* ia = dynamic_cast<IndexAccess const*>(cursor))
		{
			if (!ia->indexExpression())
				return std::nullopt;
			// Mappings are separate keyed boxes, not fields in the aggregate's
			// serialized value. Leave any path that crosses one to the mapping
			// lowerer; COW is only for recursively serializable array/struct paths.
			if (dynamic_cast<MappingType const*>(
					ia->baseExpression().annotation().type))
				return std::nullopt;
			path.push_back(cursor);
			cursor = &ia->baseExpression();
			continue;
		}
		if (auto const* ma = dynamic_cast<MemberAccess const*>(cursor);
			ma && dynamic_cast<StructType const*>(
				ma->expression().annotation().type))
		{
			path.push_back(cursor);
			cursor = &ma->expression();
			continue;
		}
		break;
	}
	if (path.empty())
		return std::nullopt;
	auto const* baseId = dynamic_cast<Identifier const*>(cursor);
	if (!baseId)
		return std::nullopt;
	auto const* vd = dynamic_cast<VariableDeclaration const*>(
		baseId->annotation().referencedDeclaration);
	if (!vd)
		return std::nullopt;
	auto const* rootArray = dynamic_cast<ArrayType const*>(vd->type());
	auto const* rootStruct = dynamic_cast<StructType const*>(vd->type());
	if ((!rootArray && !rootStruct)
		|| (rootArray && rootArray->isByteArrayOrString()))
		return std::nullopt;

	std::string keyParam = m_scope.findMappingKeyParam(vd->id());
	// Offset-carrying struct refs name a slice inside a larger box; their
	// sibling handler owns that representation. Everything here owns a complete
	// aggregate box value.
	if (!m_scope.findStructRefOffset(vd->id()).empty())
		return std::nullopt;
	auto binding = m_ctx.storageMapper.physicalBindingFor(*vd);
	bool const directBox = vd->isStateVariable()
		&& binding.kind == awst::AppStorageKind::Box;
	if (keyParam.empty() && !directBox)
		return std::nullopt;
	// Direct top-level arrays already have element-oriented box operations that
	// do not materialise the entire value (and therefore work above 4 KiB).
	// Runtime-key array refs need this COW path because their key is not static.
	if (directBox && rootArray)
		return std::nullopt;

	auto const* rootW = m_ctx.typeMapper.map(vd->type());
	if (!rootW)
		return std::nullopt;

	// Build the RHS BEFORE snapshotting the box, and when it can write state,
	// PIN it to its own pre-statement so the write actually happens first.
	// Solidity evaluates a compound assignment's RHS fully, then reads the
	// target's old value, so `s.f = 5; s.f += bump(s)` (bump does `p.f += 1`
	// and returns 100) is 6 + 100. Leaving the call inside the value
	// expression put it AFTER the snapshot statement, which read a stale 5 —
	// and worse, wrote that stale whole-struct copy back over bump's store,
	// losing the callee's write to every OTHER field too. Building earlier
	// without pinning changes nothing: the call still evaluates where the
	// expression sits.
	//
	// This is the last point where the handler can still decline; building the
	// RHS any earlier would double-evaluate it in the caller.
	auto rhs = buildExpr(m_assignment.rightHandSide());
	if (rhs && EffectScan::mayWrite(m_assignment.rightHandSide()))
	{
		std::string pinName = "__boxref_rhs_" + std::to_string(
			awst::NameGen::next("SolAssignment.boxedPathRhs"));
		auto const* pinW = rhs->wtype;
		m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(pinName, pinW, m_loc), std::move(rhs), m_loc));
		rhs = awst::makeVarExpression(pinName, pinW, m_loc);
	}

	auto makeBox = [&]() {
		std::shared_ptr<awst::Expression> key;
		if (!keyParam.empty())
			key = awst::makeReinterpretCast(
				awst::makeVarExpression(keyParam, awst::WType::bytesType(), m_loc),
				awst::WType::boxKeyType(), m_loc);
		else
			key = awst::makeUtf8BytesConstant(
				binding.name, m_loc, awst::WType::boxKeyType());
		return awst::makeBoxValueExpression(std::move(key), rootW, m_loc);
	};

	std::string rootName = "__boxref_root_" + std::to_string(
		awst::NameGen::next("SolAssignment.boxedArrayRoot"));
	m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(rootName, rootW, m_loc),
		builder::StorageMapper::makeStateGetWithDefault(makeBox(), rootW, m_loc),
		m_loc));
	std::shared_ptr<awst::Expression> target =
		awst::makeVarExpression(rootName, rootW, m_loc);
	std::reverse(path.begin(), path.end());
	for (auto const* step: path)
	{
		if (auto const* ia = dynamic_cast<IndexAccess const*>(step))
		{
			auto idx = builder::TypeCoercion::checkedIndexToUint64(
				m_ctx.preEffects(), buildExpr(*ia->indexExpression()), m_loc);
			awst::WType const* elemW = nullptr;
			if (auto const* a = dynamic_cast<awst::ARC4DynamicArray const*>(target->wtype))
				elemW = a->elementType();
			else if (auto const* a = dynamic_cast<awst::ARC4StaticArray const*>(target->wtype))
				elemW = a->elementType();
			else if (auto const* a = dynamic_cast<awst::ReferenceArray const*>(target->wtype))
				elemW = a->elementType();
			if (!elemW)
				return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc)};
			target = awst::makeIndexExpression(
				std::move(target), std::move(idx), elemW, m_loc);
			continue;
		}
		auto const* ma = dynamic_cast<MemberAccess const*>(step);
		auto const* structW = ma
			? dynamic_cast<awst::ARC4Struct const*>(target->wtype) : nullptr;
		awst::WType const* fieldW = nullptr;
		if (structW)
			for (auto const& [name, type]: structW->fields())
				if (name == ma->memberName()) { fieldW = type; break; }
		if (!ma || !fieldW)
			return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc)};
		target = awst::makeFieldExpression(
			std::move(target), ma->memberName(), fieldW, m_loc);
	}

	auto const* valueSol = m_assignment.leftHandSide().annotation().type;
	auto const* nativeW = m_ctx.typeMapper.map(valueSol);
	if (op != Token::Assign)
	{
		std::shared_ptr<awst::Expression> current = target;
		if (!awst::structurallyEquivalent(current->wtype, nativeW))
			current = awst::makeARC4Decode(
				std::move(current), nativeW, m_loc);
		rhs = widenSignedCompoundRhs(std::move(rhs));
		rhs = eb::AssignmentHelper::computeCompoundOrFallback(
			m_ctx, op, op, valueSol, std::move(current),
			std::move(rhs), nativeW, m_loc);
	}
	rhs = builder::TypeCoercion::coerceForAssignment(
		std::move(rhs), nativeW, m_loc);
	rhs = builder::TypeCoercion::signExtendSignedWiden(
		std::move(rhs), m_assignment.rightHandSide().annotation().type,
		valueSol, m_loc);
	if (!rhs)
		return std::nullopt;
	std::string valueName = "__boxref_value_" + std::to_string(
		awst::NameGen::next("SolAssignment.boxedArrayValue"));
	auto const* valueW = rhs->wtype;
	m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(valueName, valueW, m_loc), std::move(rhs), m_loc));
	std::shared_ptr<awst::Expression> stored =
		awst::makeVarExpression(valueName, valueW, m_loc);
	if (!awst::structurallyEquivalent(stored->wtype, target->wtype))
		stored = awst::makeARC4Encode(std::move(stored), target->wtype, m_loc);
	m_ctx.postEffects().push_back(awst::makeAssignmentStatement(
		std::move(target), std::move(stored), m_loc));
	m_ctx.postEffects().push_back(awst::makeExpressionStatement(
		awst::makeAssignmentExpression(
			makeBox(), awst::makeVarExpression(rootName, rootW, m_loc), m_loc, rootW),
		m_loc));
	return awst::makeVarExpression(valueName, valueW, m_loc);
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleOffsetStructRefFieldWrite()
{
	Token const op = m_assignment.assignmentOperator();

	// Peel every struct-member layer to the offset-carrying ref parameter.
	// Mutate a temporary of the complete root struct and replace that one ARC4
	// slice, which naturally handles nested structs and packed bool runs.
	std::vector<MemberAccess const*> path;
	Expression const* cursor = &m_assignment.leftHandSide();
	while (auto const* member = dynamic_cast<MemberAccess const*>(cursor))
	{
		path.push_back(member);
		cursor = &member->expression();
	}
	if (path.empty())
		return std::nullopt;
	auto const* baseId = dynamic_cast<Identifier const*>(cursor);
	if (!baseId)
		return std::nullopt;
	auto const* vd = dynamic_cast<VariableDeclaration const*>(
		baseId->annotation().referencedDeclaration);
	if (!vd)
		return std::nullopt;
	std::string offVar = m_scope.findStructRefOffset(vd->id());
	std::string keyParam = m_scope.findMappingKeyParam(vd->id());
	if (offVar.empty() || keyParam.empty())
		return std::nullopt;
	auto const* rootW = dynamic_cast<awst::ARC4Struct const*>(
		m_ctx.typeMapper.mapSolTypeToARC4(vd->type()));
	int const rootSize = builder::computeEncodedElementSize(rootW);
	if (!rootW || rootSize <= 0)
		return std::nullopt;

	auto makeKey = [&]() {
		return awst::makeReinterpretCast(
			awst::makeVarExpression(keyParam, awst::WType::bytesType(), m_loc),
			awst::WType::boxKeyType(), m_loc);
	};
	auto makeOffset = [&]() {
		return awst::makeVarExpression(
			offVar, awst::WType::uint64Type(), m_loc);
	};
	std::string rootName = "__osref_root_" + std::to_string(
		awst::NameGen::next("SolAssignment.offsetStructRoot"));
	// Build the RHS BEFORE snapshotting the root, and PIN it when it can
	// write state — the same guard tryHandleBoxedAggregatePathWrite carries
	// (see its comment): an unpinned side-effecting RHS evaluated after the
	// snapshot, and the whole-struct write-back clobbered the callee's
	// writes (`s.f += bump(s)` lost bump's `s.g += 1`; probe gave g==0).
	auto rhs = buildExpr(m_assignment.rightHandSide());
	if (rhs && EffectScan::mayWrite(m_assignment.rightHandSide()))
	{
		std::string pinName = "__osref_rhs_" + std::to_string(
			awst::NameGen::next("SolAssignment.offsetStructRhs"));
		auto const* pinW = rhs->wtype;
		m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(pinName, pinW, m_loc), std::move(rhs), m_loc));
		rhs = awst::makeVarExpression(pinName, pinW, m_loc);
	}

	auto rootBytes = awst::makeBoxExtract(
		makeKey(), makeOffset(),
		awst::makeIntegerConstant(static_cast<uint64_t>(rootSize), m_loc), m_loc);
	m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(rootName, rootW, m_loc),
		awst::makeReinterpretCast(std::move(rootBytes), rootW, m_loc), m_loc));

	std::shared_ptr<awst::Expression> target =
		awst::makeVarExpression(rootName, rootW, m_loc);
	std::reverse(path.begin(), path.end());
	for (auto const* member: path)
	{
		auto const* structW = dynamic_cast<awst::ARC4Struct const*>(target->wtype);
		awst::WType const* fieldW = nullptr;
		if (structW)
			for (auto const& [name, type]: structW->fields())
				if (name == member->memberName()) { fieldW = type; break; }
		if (!fieldW)
			return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc)};
		target = awst::makeFieldExpression(
			std::move(target), member->memberName(), fieldW, m_loc);
	}

	auto const* valueSol = m_assignment.leftHandSide().annotation().type;
	auto const* nativeW = m_ctx.typeMapper.map(valueSol);
	if (op != Token::Assign)
	{
		std::shared_ptr<awst::Expression> current = target;
		if (!awst::structurallyEquivalent(current->wtype, nativeW))
			current = awst::makeARC4Decode(
				std::move(current), nativeW, m_loc);
		rhs = widenSignedCompoundRhs(std::move(rhs));
		rhs = eb::AssignmentHelper::computeCompoundOrFallback(
			m_ctx, op, op, valueSol, std::move(current),
			std::move(rhs), nativeW, m_loc);
	}
	rhs = builder::TypeCoercion::coerceForAssignment(
		std::move(rhs), nativeW, m_loc);
	rhs = builder::TypeCoercion::signExtendSignedWiden(
		std::move(rhs), m_assignment.rightHandSide().annotation().type,
		valueSol, m_loc);
	if (!rhs)
		return std::nullopt;
	std::string valueName = "__osref_value_" + std::to_string(
		awst::NameGen::next("SolAssignment.offsetStructValue"));
	auto const* valueW = rhs->wtype;
	m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(valueName, valueW, m_loc), std::move(rhs), m_loc));
	std::shared_ptr<awst::Expression> stored =
		awst::makeVarExpression(valueName, valueW, m_loc);
	if (!awst::structurallyEquivalent(stored->wtype, target->wtype))
		stored = awst::makeARC4Encode(std::move(stored), target->wtype, m_loc);
	m_ctx.postEffects().push_back(awst::makeAssignmentStatement(
		std::move(target), std::move(stored), m_loc));
	m_ctx.postEffects().push_back(awst::makeExpressionStatement(
		awst::makeBoxReplace(makeKey(), makeOffset(), awst::makeAsBytes(
			awst::makeVarExpression(rootName, rootW, m_loc), m_loc), m_loc), m_loc));
	return awst::makeVarExpression(valueName, valueW, m_loc);
}

} // namespace puyasol::builder::sol_ast
