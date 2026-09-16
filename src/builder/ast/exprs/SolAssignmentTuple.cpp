/// @file SolAssignmentTuple.cpp — ordered tuple assignment lowering.
#include "builder/ast/exprs/SolAssignment.h"
#include "builder/eb/ResolvedLValue.h"
#include "builder/solc/SolcFacts.h"

#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/contract/ContractBuilder.h"
#include "awst/NameGen.h"
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
/// Tuple-returning call RHS (`(a,b) = f()`): cache in a temp so each TupleItem reads from the cached tuple — without snapshotting, …
std::shared_ptr<awst::Expression> SolAssignment::snapshotTupleCallRhs(
	std::shared_ptr<awst::Expression> _value)
{
	if (dynamic_cast<awst::SubroutineCallExpression const*>(_value.get())
		|| dynamic_cast<awst::SubmitInnerTransaction const*>(_value.get()))
	{
		if (dynamic_cast<awst::WTuple const*>(_value->wtype))
		{
			std::string tmpName = "__call_tuple_tmp_" + std::to_string(awst::NameGen::next("SolAssignmentTuple.s_callTupleCounter"));
			awst::WType const* tupleWtype = _value->wtype;
			auto srcLoc = _value->sourceLocation;
			auto tmpVar = awst::makeVarExpression(tmpName, tupleWtype, srcLoc);
			auto tmpAssign = awst::makeAssignmentExpression(tmpVar, _value, srcLoc);
			m_ctx.preEffects().push_back(
				awst::makeExpressionStatement(std::move(tmpAssign), srcLoc));
			_value = awst::makeVarExpression(tmpName, tupleWtype, srcLoc);
		}
	}
	return _value;
}

std::shared_ptr<awst::Expression> SolAssignment::pinLiteralTupleRhs(
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::TupleExpression const* _sourceLhs)
{
	// Literal tuple RHS: materialise the WHOLE right-hand side into temps
	// before any store — solc's rule, no conditions in it
	// (IRGeneratorForStatements::visit(Assignment) accepts the RHS before it
	// looks at the LHS; writeToLValue(IRLValue::Tuple) stores right-to-left).
	// This is what makes `(a, b) = (b, a)` a real swap and keeps side-effecting
	// items single-evaluation. Until puya's repeated-writes iterate-while-mutate
	// bug (puyabug.md §13, fixed by upstream fix/3-consecutive-write-bug) this
	// sat behind a pile of narrow triggers; the pile is gone.
	//
	// Two per-item exemptions remain, and they are the value/reference model,
	// not heuristics — an exempt item stays un-pinned so the write loop keeps
	// its in-place path:
	//  - a compile-time storage-POINTER local on the LHS (`(m, v) = (m2, 21)`):
	//    the alias rebind consumes the RHS expression itself; there is no
	//    runtime value to pin, and pinning rebound the alias to a temp.
	//  - a whole storage AGGREGATE state var on the LHS whose RHS item is a
	//    plain storage READ (`(x, y) = (y, x)` on structs): references are NOT
	//    copied into the RHS tuple, so the EVM's sequential-overwrite collapse
	//    (swap_in_storage_overwrite) is the correct semantics; pinning would
	//    materialise a copy and "fix" a swap the EVM itself does not perform.
	//    The read-only condition is load-bearing: a COMPUTED item
	//    (`(.., y, ..) = (.., returnsArray(), ..)`) must still be pinned, or
	//    its side effects run at STORE time — right-to-left, after later
	//    components' stores — instead of at RHS-evaluation time
	//    (destructuring_assignment: the deferred call re-assigned arrayData
	//    after `arrayData[3] = 2` had landed). Value types are always pinned.
	if (auto const* rhsTuple = dynamic_cast<awst::TupleExpression const*>(_value.get());
		rhsTuple && !rhsTuple->items.empty())
	{
		auto lhsComponentKeepsInPlace = [&](size_t i) -> bool {
			if (!_sourceLhs || i >= _sourceLhs->components().size())
				return false;
			auto const& comp = _sourceLhs->components()[i];
			if (!comp) return false;
			auto const* id = SolcFacts::expressionAs<solidity::frontend::Identifier>(comp.get());
			if (!id) return false;
			auto const* decl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				id->annotation().referencedDeclaration);
			if (!decl) return false;
			// Only compile-time aliases stay in place. Runtime slot/holder
			// pointers must be snapshotted before any tuple component is stored.
			if (decl->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& !decl->isStateVariable())
				return !m_ctx.typeMapper.profile().evmStorageLayout
					&& m_scope.bindings.mappingKeyParams.get(decl->id()).empty();
			// Whole aggregate state var receiving a plain storage read.
			if (decl->isStateVariable() && comp->annotation().type
				&& !comp->annotation().type->isValueType())
			{
				auto const& item = rhsTuple->items[i];
				auto peeled = awst::unwrapStateGet(item);
				if (awst::isRawStorageRead(peeled.get())
					|| dynamic_cast<awst::StateGet const*>(item.get()))
					return true;
			}
			return false;
		};

		std::vector<awst::WType const*> tmpTypes;
		auto newTuple = awst::makeTupleExpression(nullptr, _value->sourceLocation);
		for (size_t i = 0; i < rhsTuple->items.size(); ++i)
		{
			auto const& rhsItem = rhsTuple->items[i];
			tmpTypes.push_back(rhsItem->wtype);
			if (lhsComponentKeepsInPlace(i))
			{
				newTuple->items.push_back(rhsItem);
				continue;
			}
			std::string tmpName = "__tuple_tmp_" + std::to_string(awst::NameGen::next("SolAssignmentTuple.item"));
			// Must use a pre-effect (not a post-effect): post-effects insert
			// AFTER the current statement, leaving temps unassigned when the
			// bare tuple reads them — puya DCEs the assignments and leaks raw
			// call return values on the stack.
			m_ctx.preEffects().push_back(awst::makeExpressionStatement(
				awst::makeAssignmentExpression(
					awst::makeVarExpression(
						tmpName, rhsItem->wtype, _value->sourceLocation),
					rhsItem, _value->sourceLocation),
				_value->sourceLocation));
			newTuple->items.push_back(awst::makeVarExpression(
				tmpName, rhsItem->wtype, _value->sourceLocation));
		}
		newTuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(
			std::move(tmpTypes), std::nullopt);
		_value = std::move(newTuple);
	}

	return _value;
}

/// Rebind compile-time storage aliases or runtime slot/holder pointers.
SolAssignment::TupleComponentAction SolAssignment::tryStoragePointerComponent(
	size_t i,
	std::shared_ptr<awst::Expression> const& item,
	std::shared_ptr<awst::Expression> const& _value,
	solidity::frontend::TupleExpression const* _sourceLhs)
{
	// Storage-pointer in tuple `(m, v) = (m2, 21)`: the AWST target resolves
	// to the current alias (not a runtime lvalue). Update compile-time alias
	// and skip the assignment; mirrors the simple `m = m2` path.
	if (_sourceLhs && i < _sourceLhs->components().size())
	{
		auto const& comp = _sourceLhs->components()[i];
		if (comp)
		{
			auto const* lhsIdent = SolcFacts::expressionAs<solidity::frontend::Identifier>(comp.get());
			auto const* lhsDecl = lhsIdent ? dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				lhsIdent->annotation().referencedDeclaration) : nullptr;
			if (lhsDecl
				&& lhsDecl->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& !lhsDecl->isStateVariable())
			{
				auto const& keyParam = m_scope.bindings.mappingKeyParams.get(lhsDecl->id());
				if (!m_ctx.typeMapper.profile().evmStorageLayout && !keyParam.empty())
				{
					auto const* tuple = dynamic_cast<awst::WTuple const*>(_value->wtype);
					auto const* componentType = tuple && i < tuple->types().size() ? tuple->types()[i] : nullptr;
					if (componentType != awst::WType::bytesType() && componentType != awst::WType::boxKeyType())
						throw SizeError("tuple storage-reference assignment requires a runtime holder key");
					m_ctx.postEffects().push_back(awst::makeAssignmentStatement(
						awst::makeVarExpression(keyParam, awst::WType::bytesType(), m_loc),
						awst::makeAsBytes(awst::makeTupleItem(_value, static_cast<int>(i), componentType, m_loc), m_loc),
						m_loc));
					return TupleComponentAction::Handled;
				}
				// Slot mode: the local IS a runtime biguint slot handle, so a
				// tuple component re-points it with an ordinary assignment —
				// the compile-time alias below never fires there (slot-handle
				// reads don't consult the alias map), which silently dropped
				// `(a, b, c) = g()` rebinds of storage-ref returns.
				if (m_ctx.typeMapper.profile().evmStorageLayout
					|| m_scope.bindings.slotStorageRefs.get(lhsDecl->id()))
				{
					auto const* valueTuple2 =
						dynamic_cast<awst::WTuple const*>(_value->wtype);
					auto const* compW = (valueTuple2 && i < valueTuple2->types().size())
						? valueTuple2->types()[i] : nullptr;
					if (compW == awst::WType::biguintType())
					{
						// This post-effect joins the other component writes in
						// the scoped frame and is reversed with them below.
						m_ctx.postEffects().push_back(
							awst::makeAssignmentStatement(
								item,
								awst::makeTupleItem(_value, static_cast<int>(i),
									compW, m_loc),
								m_loc));
						return TupleComponentAction::Handled;
					}
					Logger::instance().error(
						"--evm-storage-layout: tuple component for storage "
						"pointer '" + lhsDecl->name()
						+ "' is not a slot handle", m_loc);
					return TupleComponentAction::Abort;
				}
				// Prefer the RHS tuple's i-th item directly: it carries the
				// BoxValueExpression/AppStateExpression needed for downstream
				// mapping-key resolution (TupleItemExpression slice loses that).
				std::shared_ptr<awst::Expression> aliasExpr;
				if (auto const* rhsTuple = dynamic_cast<awst::TupleExpression const*>(_value.get()))
				{
					if (i < rhsTuple->items.size())
						aliasExpr = rhsTuple->items[i];
				}
				if (!aliasExpr)
				{
					auto const* valueTuple = dynamic_cast<awst::WTuple const*>(_value->wtype);
					auto sliceType = (valueTuple && i < valueTuple->types().size())
						? valueTuple->types()[i] : item->wtype;
					auto slice = awst::makeTupleItem(_value, static_cast<int>(i), sliceType, m_loc);
					aliasExpr = slice;
				}
				bool wrappedStateRead = false;
				if (awst::isRawStorageRead(aliasExpr.get()))
				{
					aliasExpr = StorageMapper::makeStateGetWithDefault(aliasExpr, aliasExpr->wtype, m_loc);
					wrappedStateRead = true;
				}
				// Slice may be a raw state expression or a TupleItemExpression fallback.
				auto alias = wrappedStateRead
					|| dynamic_cast<awst::StateGet const*>(aliasExpr.get())
					? StorageAlias::stateRead(std::move(aliasExpr))
					: StorageAlias::tupleSlice(std::move(aliasExpr));
				// Same compile-time-only rebind hazard as the scalar
				// form — fail loud inside conditional regions.
				if (m_ctx.conditionalDepth > 0)
					Logger::instance().error(
						"storage-pointer reassignment inside a "
						"conditionally-executed block is not supported "
						"(compile-time rebind would apply unconditionally "
						"to all following uses).", m_loc);
				m_scope.bindings.storageAliases.set(lhsDecl->id(), std::move(alias));
				return TupleComponentAction::Handled;
			}
		}
	}

	return TupleComponentAction::NotApplicable;
}

/// Tuple stores share the scalar/array representation conversion used by plain stores.
void SolAssignment::coerceTupleComponentValue(
	std::shared_ptr<awst::Expression> const& assignTarget,
	std::shared_ptr<awst::Expression>& assignValue)
{
	assignValue = builder::TypeCoercion::coerceScalar(
		std::move(assignValue), assignTarget->wtype, m_loc);
	assignValue = eb::AssignmentHelper::arc4EncodeForType(
		m_ctx, std::move(assignValue), assignTarget->wtype, m_loc);
	assignValue = builder::TypeCoercion::coerceForAssignment(
		std::move(assignValue), assignTarget->wtype, m_loc, &m_ctx.preEffects());
}

/// Emit one tuple component's write (post-effects; GroupMark closes the component's statement group even on early returns).
bool SolAssignment::emitTupleComponentWrite(
	size_t i,
	std::shared_ptr<awst::Expression> const& itemIn,
	std::shared_ptr<awst::Expression> const& _value,
	solidity::frontend::TupleExpression const* _sourceLhs,
	solidity::frontend::TupleType const* _sourceType,
	std::vector<size_t>& componentGroupEnds)
{
	struct GroupMark
	{
		std::vector<size_t>& ends;
		eb::ContractContext& ctx;
		~GroupMark() { ends.push_back(ctx.postEffects().size()); }
	} groupMark{componentGroupEnds, m_ctx};
	auto item = itemIn;

	// Skip null placeholders (empty-name VarExpression for gaps like `(,,a)`)
	if (auto const* varExpr = dynamic_cast<awst::VarExpression const*>(item.get()))
		if (varExpr->name.empty())
			return true;

	// A tuple component can rebind a pointer-backed memory local just like a
	// plain `p = value` assignment. The generic tuple target is a materialized
	// value (not an lvalue), so repoint its registered offset explicitly. This
	// keeps tuple rebinding runtime-scoped inside branches and loops.
	if (_sourceLhs && i < _sourceLhs->components().size()
		&& _sourceLhs->components()[i])
	{
		auto const* identifier = SolcFacts::expressionAs<Identifier>(&SolcFacts::functionExpression(*_sourceLhs->components()[i]));
		auto const* declaration = identifier
			? dynamic_cast<VariableDeclaration const*>(
				identifier->annotation().referencedDeclaration)
			: nullptr;
		std::string const offsetName = declaration
			? m_scope.bindings.blobAggregates.get(declaration->id()) : std::string{};
		if (declaration && !offsetName.empty()
			&& declaration->referenceLocation()
				== VariableDeclaration::Location::Memory)
		{
			auto const* valueTuple = dynamic_cast<awst::WTuple const*>(
				_value->wtype);
			auto const* componentType = valueTuple
				&& i < valueTuple->types().size()
				? valueTuple->types()[i]
				: m_ctx.typeMapper.map(declaration->type());
			std::shared_ptr<awst::Expression> value = awst::makeTupleItem(
				_value, static_cast<int>(i), componentType, m_loc);
			if (componentType == awst::WType::uint64Type())
			{
				m_ctx.postEffects().push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(offsetName, componentType, m_loc), std::move(value), m_loc));
				return true;
			}
			auto const* targetType = m_ctx.typeMapper.map(declaration->type());
			auto copy = m_ctx.lowerOperand([&] {
				assert(_sourceType);
				value = EvmSlotLowering::materializeRefValue(m_ctx, m_scope,
					std::move(value), _sourceType->components()[i], targetType, m_loc);
				value = ConversionPlan{_sourceType->components()[i], declaration->type(), targetType,
					ConversionPlan::Context::Assignment}.emit(std::move(value), m_loc, &m_ctx.preEffects());
				return builder::emitBlobBackValue(
					m_ctx.typeMapper, declaration->type(), targetType,
					std::move(value), offsetName,
					awst::NameGen::next("SolAssignmentTuple.blobRespill"),
					m_loc, m_ctx.postEffects());
			}, false);
			for (auto& statement: copy.effects.pre) m_ctx.queuePostEffect(std::move(statement));
			for (auto& statement: copy.effects.post) m_ctx.queuePostEffect(std::move(statement));
			return true;
		}
	}

	switch (tryStoragePointerComponent(i, item, _value, _sourceLhs))
	{
	case TupleComponentAction::Handled: return true;
	case TupleComponentAction::Abort: return false;
	case TupleComponentAction::NotApplicable: break;
	}

	// Use value tuple's element type (not the target's)
	auto const* valueTuple = dynamic_cast<awst::WTuple const*>(_value->wtype);
	auto const* itemWtype = (valueTuple && i < valueTuple->types().size())
		? valueTuple->types()[i] : item->wtype;
	auto itemExpr = awst::makeTupleItem(_value, static_cast<int>(i), itemWtype, m_loc);

	auto assignTarget = item;
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(item.get()))
		assignTarget = decodeExpr->value;
	assignTarget = awst::unwrapStateGet(std::move(assignTarget));

	std::shared_ptr<awst::Expression> assignValue = std::move(itemExpr);
	if (dynamic_cast<awst::TupleExpression const*>(assignTarget.get()))
	{
		auto const* nested = _sourceLhs ? SolcFacts::expressionAs<TupleExpression>(_sourceLhs->components()[i].get()) : nullptr;
		handleTupleAssignment(assignTarget, std::move(assignValue), nested,
			_sourceType ? dynamic_cast<TupleType const*>(_sourceType->components()[i]) : nullptr);
		return true;
	}
	if (!_sourceLhs) coerceTupleComponentValue(assignTarget, assignValue);

	// The component's store is one ordered group. Address effects were emitted
	// after the RHS snapshot; read/encode/COW/write effects stay with this store.
	if (_sourceLhs && _sourceLhs->components()[i])
	{
		auto const& source = SolcFacts::unparenthesized(*_sourceLhs->components()[i]);
		auto lowered = m_ctx.lowerOperand([&] {
			assert(_sourceType);
			auto const* targetType = source.annotation().type;
			auto const* native = m_ctx.typeMapper.map(targetType);
			assignValue = EvmSlotLowering::materializeRefValue(m_ctx, m_scope,
				std::move(assignValue), _sourceType->components()[i], native, m_loc);
			assignValue = ConversionPlan{_sourceType->components()[i], targetType,
				native, ConversionPlan::Context::Assignment}.emit(
					std::move(assignValue), m_loc, &m_ctx.preEffects());
			auto resolved = m_tupleTargets.find(source.id());
			if (resolved != m_tupleTargets.end()) return resolved->second->write(assignValue);
			ResolvedLValue target(m_ctx, source, m_loc, assignTarget);
			return target.write(assignValue);
		}, false);
		for (auto& stmt: lowered.effects.pre) m_ctx.queuePostEffect(std::move(stmt));
		for (auto& stmt: lowered.effects.post) m_ctx.queuePostEffect(std::move(stmt));
	}
	else
		m_ctx.queuePostExpression(awst::makeAssignmentExpression(
			std::move(assignTarget), std::move(assignValue), m_loc), m_loc);
	return true;
}

std::shared_ptr<awst::Expression> SolAssignment::handleTupleAssignment(
	std::shared_ptr<awst::Expression> _target,
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::TupleExpression const* _sourceLhs,
	solidity::frontend::TupleType const* _sourceType)
{
	if (!_sourceType) _sourceType = dynamic_cast<TupleType const*>(m_assignment.rightHandSide().annotation().type);
	auto const* tupleTarget = dynamic_cast<awst::TupleExpression const*>(_target.get());
	auto const& items = tupleTarget->items;

	// Build tuple writes in their own structural effect frame. Only the writes
	// produced by this destructure are reversed; unrelated parent effects never
	// participate in snapshot/tail arithmetic.
	//
	// componentGroupEnds records where each component's contribution ends, so the
	// right-to-left reversal below can flip COMPONENTS without scrambling the
	// statements inside one. Recorded by a scope guard because the loop body
	// exits from several places (emitTupleComponentWrite).
	std::vector<size_t> componentGroupEnds;
	auto writes = m_ctx.lowerOperand([&]() -> bool {
		for (size_t i = 0; i < items.size(); ++i)
			if (!emitTupleComponentWrite(
					i, items[i], _value, _sourceLhs, _sourceType, componentGroupEnds))
				return false;
		return true;
		}, false);
	if (!writes.value)
		return nullptr;

	// Reverse to right-to-left (Solidity viaYul: last element stored first) —
	// by COMPONENT, not by statement. One component can lower to several
	// statements whose internal order matters: a slot-mode packed address pins
	// its slot and its value, then writes the aux slot and the word. A flat
	// reverse put those writes BEFORE the pins that feed them, and the call died
	// on an unassigned slot ("b% arg 0 wanted bigint but got uint64"). Where
	// every component contributes one statement this is exactly the old flat
	// reverse.
	{
		std::vector<std::pair<size_t, size_t>> groups;
		size_t groupStart = 0;
		for (size_t groupEnd: componentGroupEnds)
		{
			if (groupEnd > groupStart)
				groups.emplace_back(groupStart, groupEnd);
			groupStart = groupEnd;
		}
		if (groupStart < writes.effects.post.size())
			groups.emplace_back(groupStart, writes.effects.post.size());

		std::vector<std::shared_ptr<awst::Statement>> ordered;
		ordered.reserve(writes.effects.post.size());
		for (auto group = groups.rbegin(); group != groups.rend(); ++group)
			for (size_t k = group->first; k < group->second; ++k)
				ordered.push_back(std::move(writes.effects.post[k]));
		writes.effects.post = std::move(ordered);
	}
	m_ctx.restoreOperandDeltas(std::move(writes.effects));

	return _value;
}

} // namespace puyasol::builder::sol_ast
