/// @file SolAssignmentTuple.cpp — handleTupleAssignment + buildTupleWithUpdatedField
#include "builder/sol-ast/exprs/SolAssignment.h"

#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "awst/NameGen.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

std::shared_ptr<awst::Expression> SolAssignment::buildTupleWithUpdatedField(
	std::shared_ptr<awst::Expression> _base,
	std::string const& _fieldName,
	std::shared_ptr<awst::Expression> _newValue)
{
	auto const* tupleType = dynamic_cast<awst::WTuple const*>(_base->wtype);
	auto const& names = *tupleType->names();
	auto const& types = tupleType->types();

	auto tuple = awst::makeTupleExpression(_base->wtype, m_loc);

	for (size_t i = 0; i < names.size(); ++i)
	{
		if (names[i] == _fieldName)
			tuple->items.push_back(std::move(_newValue));
		else
		{
			auto field = awst::makeFieldExpression(_base, names[i], types[i], m_loc);
			tuple->items.push_back(std::move(field));
		}
	}
	return tuple;
}

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
			auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(comp.get());
			if (!id) return false;
			auto const* decl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				id->annotation().referencedDeclaration);
			if (!decl) return false;
			// Storage-pointer local: the alias branch owns it.
			if (decl->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& !decl->isStateVariable())
				return true;
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
			std::string tmpName = "__tuple_tmp_" + std::to_string(m_loc.line)
				+ "_" + std::to_string(i);
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

/// Storage-pointer / slot-struct tuple component: compile-time alias rebind (or the slot-handle re-point / slot-level struct copy …
SolAssignment::TupleComponentAction SolAssignment::tryStoragePointerComponent(
	size_t i,
	std::shared_ptr<awst::Expression> const& item,
	std::shared_ptr<awst::Expression> const& _value,
	solidity::frontend::TupleExpression const* _sourceLhs)
{
	(void) item;
	// Storage-pointer in tuple `(m, v) = (m2, 21)`: the AWST target resolves
	// to the current alias (not a runtime lvalue). Update compile-time alias
	// and skip the assignment; mirrors the simple `m = m2` path.
	if (_sourceLhs && i < _sourceLhs->components().size())
	{
		auto const& comp = _sourceLhs->components()[i];
		if (comp)
		{
			auto const* lhsIdent = dynamic_cast<solidity::frontend::Identifier const*>(comp.get());
			auto const* lhsDecl = lhsIdent ? dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				lhsIdent->annotation().referencedDeclaration) : nullptr;
			if (lhsDecl
				&& lhsDecl->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Storage
				&& !lhsDecl->isStateVariable())
			{
				// Slot mode: the local IS a runtime biguint slot handle, so a
				// tuple component re-points it with an ordinary assignment —
				// the compile-time alias below never fires there (slot-handle
				// reads don't consult the alias map), which silently dropped
				// `(a, b, c) = g()` rebinds of storage-ref returns.
				if (m_ctx.typeMapper.profile().evmStorageLayout)
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
								awst::makeVarExpression(lhsDecl->name(),
									awst::WType::biguintType(), m_loc),
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
				m_scope.setStorageAlias(lhsDecl->id(), std::move(alias));
				return TupleComponentAction::Handled;
			}

			// Slot mode, storage STRUCT ← storage STRUCT component: emit a
			// slot-level copy with POINTER semantics — no snapshot. The tail
			// reversal below then orders components right-to-left, which is
			// exactly Solidity's storage-tuple quirk: `(x, y) = (y, x)` is
			// `y = x; x = y` (swap_in_storage_overwrite pins it).
			if (m_ctx.typeMapper.profile().evmStorageLayout)
			{
				auto const* lst = dynamic_cast<solidity::frontend::StructType const*>(
					comp->annotation().type);
				auto const* rhsTupExpr = dynamic_cast<solidity::frontend::TupleExpression const*>(
					&m_assignment.rightHandSide());
				solidity::frontend::Expression const* rcomp =
					(rhsTupExpr && i < rhsTupExpr->components().size()
						&& rhsTupExpr->components()[i])
					? rhsTupExpr->components()[i].get() : nullptr;
				auto const* rst = rcomp ? dynamic_cast<solidity::frontend::StructType const*>(
					rcomp->annotation().type) : nullptr;
				if (lst && rst
					&& &lst->structDefinition() == &rst->structDefinition()
					&& lst->storageSize() <= 64
					&& EvmSlotLowering::isStorageStateRef(*comp)
					&& EvmSlotLowering::isStorageStateRef(*rcomp))
				{
					EvmSlotLowering low(m_ctx, m_scope, m_loc);
					auto la = low.resolve(*comp);
					auto ra = low.resolve(*rcomp);
					if (la && ra)
					{
						unsigned slots = static_cast<unsigned>(lst->storageSize());
						// NAMED pins, not EvalOnce: the copy spans several
						// statements and the tail reversal reorders them —
						// cross-statement SingleEvaluation reuse is the
						// known SE-dominance hazard.
						int swpId = awst::NameGen::next("SolAssignmentTuple.swp");
						std::string lname = "__swp_l_" + std::to_string(swpId);
						std::string rname = "__swp_r_" + std::to_string(swpId);
						m_ctx.preEffects().push_back(
							awst::makeAssignmentStatement(
								awst::makeVarExpression(lname,
									awst::WType::biguintType(), m_loc),
								la->slot, m_loc));
						m_ctx.preEffects().push_back(
							awst::makeAssignmentStatement(
								awst::makeVarExpression(rname,
									awst::WType::biguintType(), m_loc),
								ra->slot, m_loc));
						auto lslot = awst::makeVarExpression(
							lname, awst::WType::biguintType(), m_loc);
						auto rslot = awst::makeVarExpression(
							rname, awst::WType::biguintType(), m_loc);
						for (unsigned j = 0; j < slots; ++j)
						{
							auto jc = [&]() {
								return awst::makeIntegerConstant(
									j, m_loc, awst::WType::biguintType());
							};
							auto dst = awst::makeBigUIntBinOp(lslot,
								awst::BigUIntBinaryOperator::Add, jc(), m_loc);
							auto src = awst::makeBigUIntBinOp(rslot,
								awst::BigUIntBinaryOperator::Add, jc(), m_loc);
							m_ctx.postEffects().push_back(
								builder::SlotHandleAccess::writeSlot(std::move(dst),
									builder::SlotHandleAccess::readSlot(
										std::move(src), m_loc), m_loc));
						}
						return TupleComponentAction::Handled;
					}
				}
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
	assignValue = builder::TypeCoercion::implicitNumericCast(
		std::move(assignValue), assignTarget->wtype, m_loc);
	assignValue = eb::AssignmentHelper::arc4EncodeForTarget(
		m_ctx, std::move(assignValue), assignTarget, m_loc);
	assignValue = builder::TypeCoercion::coerceForAssignment(
		std::move(assignValue), assignTarget->wtype, m_loc, &m_ctx.preEffects());
}

/// Emit one tuple component's write (post-effects; GroupMark closes the component's statement group even on early returns).
bool SolAssignment::emitTupleComponentWrite(
	size_t i,
	std::shared_ptr<awst::Expression> const& itemIn,
	std::shared_ptr<awst::Expression> const& _value,
	solidity::frontend::TupleExpression const* _sourceLhs,
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
	coerceTupleComponentValue(assignTarget, assignValue);

	// ARC4Struct field: COW via struct field handler.
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(assignTarget.get()))
	{
		auto const* structType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (!structType)
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
				structType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);

		if (structType)
		{
			// _emitAsStatement=true: helper queues the COW store. Without this,
			// `(s.a, s.b) = f()` computed f() but never wrote the fields
			// (previously mis-attributed to a puya DCE bug; see [[uros-multireturn-struct-destructure-dce]]).
			auto result = handleStructFieldAssignment(
				fieldExpr, std::move(assignValue), assignTarget, /*_emitAsStatement=*/true);
			if (result) return true;
		}
	}

	if (assignTarget->wtype != assignValue->wtype
		&& assignTarget->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		auto enc = awst::makeARC4Encode(std::move(assignValue), assignTarget->wtype, m_loc);
		assignValue = std::move(enc);
	}

	// Nested tuple: recursively destructure instead of a direct assignment.
	if (dynamic_cast<awst::TupleExpression const*>(assignTarget.get()))
	{
		handleTupleAssignment(assignTarget, std::move(assignValue));
		return true;
	}

	// Transient var: route through TransientStorage (scratch-slot blob);
	// an AssignmentExpression targeting a ReinterpretCast isn't an lvalue in puya.
	if (_sourceLhs && m_ctx.transientStorage
		&& i < _sourceLhs->components().size() && _sourceLhs->components()[i])
	{
		if (auto const* srcIdent = dynamic_cast<solidity::frontend::Identifier const*>(
				_sourceLhs->components()[i].get()))
		{
			auto const* srcDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				srcIdent->annotation().referencedDeclaration);
			if (srcDecl && srcDecl->isStateVariable()
				&& srcDecl->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Transient
				&& m_ctx.transientStorage->isTransient(*srcDecl))
			{
				auto* varType = m_ctx.typeMapper.map(srcDecl->type());
				auto coerced = builder::TypeCoercion::coerceForAssignment(
					std::move(assignValue), varType, m_loc);
				auto stmt = m_ctx.transientStorage->buildWrite(*srcDecl, coerced, m_loc);
				if (stmt)
					m_ctx.postEffects().push_back(std::move(stmt));
				return true;
			}
		}
	}

	// --evm-storage-layout: a storage element/field has no AWST lvalue —
	// building the LHS lowered it to a __storage_read CALL, which then sat
	// in the assignment's TARGET position and puya rejected the whole AWST
	// ("deserialization failed: 'SubroutineCallExpression'", 9 fixtures).
	// Route these through the slot writer exactly like the scalar path in
	// SolAssignment does.
	if (m_ctx.typeMapper.profile().evmStorageLayout && _sourceLhs
		&& i < _sourceLhs->components().size() && _sourceLhs->components()[i])
	{
		auto const& lhsComp = *_sourceLhs->components()[i];
		auto const* compType = lhsComp.annotation().type;
		if (compType && EvmSlotLowering::isStorageStateRef(lhsComp))
		{
			EvmSlotLowering low(m_ctx, m_scope, m_loc);
			if (auto addr = low.resolve(lhsComp))
			{
				std::vector<std::shared_ptr<awst::Statement>> slotOut;
				if (low.writeAny(*addr, compType, assignValue, slotOut))
				{
					for (auto& st: slotOut)
						m_ctx.postEffects().push_back(std::move(st));
					return true;
				}
			}
		}
	}
	auto e = awst::makeAssignmentExpression(
		std::move(assignTarget), std::move(assignValue), m_loc);
	m_ctx.queuePostExpression(e, m_loc);
	return true;
}

std::shared_ptr<awst::Expression> SolAssignment::handleTupleAssignment(
	std::shared_ptr<awst::Expression> _target,
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::TupleExpression const* _sourceLhs)
{
	auto const* tupleTarget = dynamic_cast<awst::TupleExpression const*>(_target.get());
	auto const& items = tupleTarget->items;

	_value = snapshotTupleCallRhs(std::move(_value));

	_value = pinLiteralTupleRhs(std::move(_value), _sourceLhs);

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
					i, items[i], _value, _sourceLhs, componentGroupEnds))
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
