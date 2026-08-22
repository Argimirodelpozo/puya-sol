/// @file SolAssignmentTuple.cpp — handleTupleAssignment + buildTupleWithUpdatedField
#include "builder/sol-ast/exprs/SolAssignment.h"

#include <libsolidity/ast/ASTVisitor.h>
#include <set>
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "awst/NameGen.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4ArrayWidening.h"
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

std::shared_ptr<awst::Expression> SolAssignment::handleTupleAssignment(
	std::shared_ptr<awst::Expression> _target,
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::TupleExpression const* _sourceLhs)
{
	auto const* tupleTarget = dynamic_cast<awst::TupleExpression const*>(_target.get());
	auto const& items = tupleTarget->items;

	// Tuple-returning call RHS (`(a,b) = f()`): without snapshotting, each
	// TupleItemExpression carries a fresh SubroutineCallExpression and puya
	// re-emits the call once per element. Cache in a temp so each TupleItem
	// reads from the cached tuple. (Analogue of the literal `(a(),b())` snapshot below.)
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

	// Literal tuple RHS of VarExpressions: snapshot into temps so `(a,b) = (b,a)`
	// doesn't reduce to `a=b; b=a` (lazy refs). Limited to pure VarExpression
	// items so storage-var tuples keep the (EVM-documented) in-place semantics
	// that swap_in_storage_overwrite relies on.
	if (auto const* rhsTuple = dynamic_cast<awst::TupleExpression const*>(_value.get()))
	{
		bool allLocalVars = !rhsTuple->items.empty();
		for (auto const& it : rhsTuple->items)
		{
			auto const* ve = dynamic_cast<awst::VarExpression const*>(it.get());
			if (!ve || ve->name.empty())
			{
				allLocalVars = false;
				break;
			}
		}
		// Also snapshot scalar RHS when LHS has a transient var: transient writes
		// go through a subroutine (packed scratch blob) that can clobber other
		// transient reads in the same tuple. Aggregates keep in-place semantics.
		bool hasTransientLhs = false;
		if (_sourceLhs && m_ctx.transientStorage)
		{
			for (auto const& comp : _sourceLhs->components())
			{
				if (!comp) continue;
				auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(comp.get());
				if (!id) continue;
				auto const* d = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
					id->annotation().referencedDeclaration);
				if (d && d->isStateVariable()
					&& d->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Transient
					&& m_ctx.transientStorage->isTransient(*d))
				{
					hasTransientLhs = true;
					break;
				}
			}
		}
		bool allScalars = hasTransientLhs && !rhsTuple->items.empty();
		for (auto const& it : rhsTuple->items)
		{
			if (!allScalars) break;
			auto const* t = it->wtype;
			if (!t) { allScalars = false; break; }
			bool scalar = (t == awst::WType::uint64Type()
				|| t == awst::WType::biguintType()
				|| t == awst::WType::boolType()
				|| t == awst::WType::accountType()
				|| t == awst::WType::applicationType()
				|| t == awst::WType::assetType()
				|| t == awst::WType::stringType()
				|| t == awst::WType::bytesType()
				|| t->kind() == awst::WTypeKind::ARC4UIntN
				|| t->kind() == awst::WTypeKind::Bytes);
			if (!scalar) { allScalars = false; break; }
		}
		// Side-effecting RHS + state-var-index LHS: each LHS re-evaluates the
		// TupleItemExpression base; repeated calls (e.g. `returnsArray()`) clobber
		// prior writes to `arrayData[3]`. Snapshot RHS to temps.
		// Guard: LHS must index state — avoids the (y,y,y)=(set(1),set(2),set(3))
		// pattern where inlined temps interact badly with puya's optimizer (leaked
		// stack values from redundant assignments).
		bool hasSideEffectingRhs = false;
		for (auto const& it : rhsTuple->items)
		{
			if (dynamic_cast<awst::SubroutineCallExpression const*>(it.get())
				|| dynamic_cast<awst::IntrinsicCall const*>(it.get())
				|| dynamic_cast<awst::SubmitInnerTransaction const*>(it.get())
				|| dynamic_cast<awst::CreateInnerTransaction const*>(it.get()))
			{
				hasSideEffectingRhs = true;
				break;
			}
		}
		bool lhsHasStateIndex = false;
		if (hasSideEffectingRhs)
		{
			auto const* lhsTuple = dynamic_cast<awst::TupleExpression const*>(_target.get());
			if (lhsTuple)
			{
				for (auto const& it : lhsTuple->items)
				{
							// IndexExpression(StateGet(BoxValue)/AppState): write to a state-array
					// element where re-evaluating RHS clobbers prior writes.
					auto const* idx = dynamic_cast<awst::IndexExpression const*>(it.get());
					if (!idx) continue;
					auto const* base = idx->base.get();
					if (auto const* sg = dynamic_cast<awst::StateGet const*>(base))
						base = sg->field.get();
					if (awst::isRawStorageRead(base))
					{
						lhsHasStateIndex = true;
						break;
					}
				}
			}
		}
		// Compound-lvalue LHS (`(arr[i],arr[j])=(arr[j],arr[i])`, `(s.a,s.b)=
		// (s.b,s.a)`, `(m[k1],m[k2])=(m[k2],m[k1])`): a parallel tuple assignment
		// must evaluate the whole RHS BEFORE any store, but the lazy RHS instead
		// reduces a swap to sequential `t2=t1; t1=t2` — both targets collapse to
		// one source value (memory element: the write reassigns the whole backing
		// blob local; storage element/field/mapping: the in-place box write is read
		// back by the next lazy RHS read). Snapshot the RHS into temps to restore
		// EVM parallel semantics. Covers array elements (ArrayType index), struct
		// value fields (MemberAccess), and mapping elements (MappingType index),
		// storage AND memory.
		//
		// A plain Identifier component needs the same snapshot when it names a
		// VALUE type AND the RHS reads it back. Solidity COPIES a value type into
		// the RHS tuple, so `uint256 a, b; (a, b) = (b, a)` really swaps; only an
		// AGGREGATE storage var is a reference, and only that keeps the
		// sequential-overwrite collapse — swap_in_storage_overwrite, the fixture
		// this exemption cites, swaps two STRUCTS. Keying on "component is an
		// Identifier" swept the value-type case in with it and silently collapsed
		// every value-type swap to (b, b). Nothing in the suite covered it;
		// test_tuple_swap_value_state_vars now does.
		//
		// ── Why this is a gate at all ──
		// Solidity's rule has no conditions in it. solc materialises the WHOLE RHS
		// and then stores components right-to-left:
		// IRGeneratorForStatements::visit(Assignment) accepts the right-hand side
		// before it even looks at the LHS, and writeToLValue(IRLValue::Tuple) walks
		// components in reverse. No dependency analysis, no exemption list — the
		// storage-aggregate collapse is EMERGENT, because a reference type's temp
		// names a slot, so the second copy reads storage the first already
		// overwrote.
		//
		// We cannot copy that yet. Materialising unconditionally produces repeated
		// stores to one key, and puya's O2 dead-store elimination removes the
		// overwritten stores while LEAVING THEIR VALUES on the stack, where a later
		// expression consumes one as an operand (puyabug.md §13 — correct at -O0,
		// wrong at -O2; it reproduces in plain sequential Solidity with no tuple
		// anywhere). So the conditions below exist to avoid emitting that shape, not
		// because the language asks for them.
		//
		// Hence the read-back bound, and both halves are load-bearing:
		// `(y, y, y) = (set(1), set(2), set(3))` writes a value type but never reads
		// one, and snapshotting there walks straight into §13;
		// `(m, v) = (m2, 21)` has a value-type `v`, but the RHS reads only `m2`, and
		// snapshotting rebinds the mapping alias to a temp so later writes miss.
		//
		// It is a bound, not a cure: a repeated target WITH a read-back still hits
		// §13 (test_tuple_duplicate_target_puya_o2, xfail). When §13 is fixed
		// upstream, delete this whole gate and materialise unconditionally.
		bool lhsNeedsRhsSnapshot = false;
		std::set<int64_t> valueTypeTargets;
		if (_sourceLhs)
		{
			for (auto const& comp : _sourceLhs->components())
			{
				if (!comp) continue;
				if (auto const* lhsId =
						dynamic_cast<solidity::frontend::Identifier const*>(comp.get()))
				{
					auto const* compType = comp->annotation().type;
					if (compType && compType->isValueType())
						if (auto const* decl = lhsId->annotation().referencedDeclaration)
							valueTypeTargets.insert(decl->id());
					continue;
				}
				if (dynamic_cast<solidity::frontend::MemberAccess const*>(comp.get()))
				{
					lhsNeedsRhsSnapshot = true;
					break;
				}
				auto const* ia = dynamic_cast<solidity::frontend::IndexAccess const*>(comp.get());
				if (!ia) continue;
				auto const* baseType = ia->baseExpression().annotation().type;
				if (dynamic_cast<solidity::frontend::ArrayType const*>(baseType)
					|| dynamic_cast<solidity::frontend::MappingType const*>(baseType))
				{
					lhsNeedsRhsSnapshot = true;
					break;
				}
			}
		}
		if (!lhsNeedsRhsSnapshot && !valueTypeTargets.empty())
		{
			struct ReadsTarget: solidity::frontend::ASTConstVisitor
			{
				std::set<int64_t> const& targets;
				bool found = false;
				explicit ReadsTarget(std::set<int64_t> const& _targets): targets(_targets) {}
				bool visit(solidity::frontend::Identifier const& _id) override
				{
					if (auto const* decl = _id.annotation().referencedDeclaration)
						if (targets.count(decl->id()))
							found = true;
					return !found;
				}
			} readsTarget{valueTypeTargets};
			m_assignment.rightHandSide().accept(readsTarget);
			lhsNeedsRhsSnapshot = readsTarget.found;
		}
		if (allLocalVars || allScalars || lhsNeedsRhsSnapshot || (hasSideEffectingRhs && lhsHasStateIndex))
		{
			std::vector<awst::WType const*> tmpTypes;
			auto newTuple = awst::makeTupleExpression(nullptr, _value->sourceLocation);
			for (size_t i = 0; i < rhsTuple->items.size(); ++i)
			{
				auto const& rhsItem = rhsTuple->items[i];
				std::string tmpName = "__tuple_tmp_" + std::to_string(m_loc.line)
					+ "_" + std::to_string(i);

				auto tmpTarget = awst::makeVarExpression(tmpName, rhsItem->wtype, _value->sourceLocation);

				auto tmpAssign = awst::makeAssignmentExpression(
					tmpTarget, rhsItem, _value->sourceLocation);

				auto stmt = awst::makeExpressionStatement(std::move(tmpAssign), _value->sourceLocation);
				// Must use a pre-effect (not a post-effect): post-effects
				// inserts AFTER the current statement, leaving temps unassigned
				// when the bare tuple reads them — puya DCEs the assignments and
				// leaks raw call return values on the stack.
				m_ctx.preEffects().push_back(std::move(stmt));

				auto tmpRead = awst::makeVarExpression(tmpName, rhsItem->wtype, _value->sourceLocation);
				newTuple->items.push_back(std::move(tmpRead));
				tmpTypes.push_back(rhsItem->wtype);
			}
			newTuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(
				std::move(tmpTypes), std::nullopt);
			_value = std::move(newTuple);
		}
	}

	// Build tuple writes in their own structural effect frame. Only the writes
	// produced by this destructure are reversed; unrelated parent effects never
	// participate in snapshot/tail arithmetic.
	//
	// componentGroupEnds records where each component's contribution ends, so the
	// right-to-left reversal below can flip COMPONENTS without scrambling the
	// statements inside one. Recorded by a scope guard because the loop body
	// `continue`s from several places.
	std::vector<size_t> componentGroupEnds;
	auto writes = m_ctx.lowerOperand([&]() -> bool {
	for (size_t i = 0; i < items.size(); ++i)
	{
		struct GroupMark
		{
			std::vector<size_t>& ends;
			eb::ContractContext& ctx;
			~GroupMark() { ends.push_back(ctx.postEffects().size()); }
		} groupMark{componentGroupEnds, m_ctx};
		auto item = items[i];

		// Skip null placeholders (empty-name VarExpression for gaps like `(,,a)`)
		if (auto const* varExpr = dynamic_cast<awst::VarExpression const*>(item.get()))
			if (varExpr->name.empty())
				continue;

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
							continue;
						}
						Logger::instance().error(
							"--evm-storage-layout: tuple component for storage "
							"pointer '" + lhsDecl->name()
							+ "' is not a slot handle", m_loc);
						return false;
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
					continue;
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
							continue;
						}
					}
				}
			}
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
		// Coerce string↔bytes
		if (assignTarget->wtype != assignValue->wtype)
		{
			bool srcIsStringOrBytes = assignValue->wtype == awst::WType::stringType()
				|| assignValue->wtype == awst::WType::bytesType()
				|| (assignValue->wtype && assignValue->wtype->kind() == awst::WTypeKind::Bytes);
			bool tgtIsStringOrBytes = assignTarget->wtype == awst::WType::stringType()
				|| assignTarget->wtype == awst::WType::bytesType()
				|| (assignTarget->wtype && assignTarget->wtype->kind() == awst::WTypeKind::Bytes);
			if (srcIsStringOrBytes && tgtIsStringOrBytes)
			{
				auto cast = awst::makeReinterpretCast(std::move(assignValue), assignTarget->wtype, m_loc);
				assignValue = std::move(cast);
			}
			else
			{
				assignValue = builder::TypeCoercion::implicitNumericCast(
					std::move(assignValue), assignTarget->wtype, m_loc);
			}
		}
		if (assignTarget->wtype != assignValue->wtype)
		{
			bool const targetIsArc4 =
				builder::isArc4EncodedType(assignTarget->wtype);
			if (targetIsArc4)
			{
				assignValue = builder::TypeCoercion::stringToBytes(std::move(assignValue), m_loc);
				bool handled = false;
				// ARC4 array element widening (intM → intN, M<N): pin source bytes to
				// a temp (helper reads them multiple times).
				bool const sourceIsArc4Array =
					assignValue->wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| assignValue->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
				bool const targetIsArc4Array =
					assignTarget->wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| assignTarget->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
				if (sourceIsArc4Array && targetIsArc4Array)
				{
					std::string tmpName = "__widen_src_h_" + std::to_string(awst::NameGen::next("SolAssignmentTuple.s_widCounter"));
					auto srcAsBytes = awst::makeAsBytes(assignValue, m_loc);
					auto tmpVar = awst::makeVarExpression(
						tmpName, awst::WType::bytesType(), m_loc);
					m_ctx.preEffects().push_back(
						awst::makeAssignmentStatement(tmpVar, std::move(srcAsBytes), m_loc));
					auto const* sourceType = assignValue->wtype;
					auto mkSrc = [&]() {
						return awst::makeVarExpression(
							tmpName, awst::WType::bytesType(), m_loc);
					};
					std::shared_ptr<awst::Expression> widened;
					if (assignTarget->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
					{
						widened = builder::tryWidenArc4StaticArrayInt(
							sourceType, assignTarget->wtype, mkSrc, m_loc);
					}
					else
					{
						widened = builder::tryWidenArc4DynamicArrayInt(
							sourceType, assignTarget->wtype, mkSrc,
							[this](std::shared_ptr<awst::Statement> _s) {
								m_ctx.preEffects().push_back(std::move(_s));
							},
							m_loc);
					}
					if (widened)
					{
						assignValue = std::move(widened);
						handled = true;
					}
				}
				// Narrowing: uint64 → arc4.uintN where N < 64.
				if (!handled)
				{
					if (auto narrowed = builder::tryNarrowUInt64ToArc4UIntN(
							assignValue, assignTarget->wtype, m_loc))
					{
						assignValue = std::move(narrowed);
						handled = true;
					}
				}
				if (!handled)
				{
					auto encode = awst::makeARC4Encode(std::move(assignValue), assignTarget->wtype, m_loc);
					assignValue = std::move(encode);
				}
			}
			else
				assignValue = builder::TypeCoercion::implicitNumericCast(
					std::move(assignValue), assignTarget->wtype, m_loc);
		}

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
				if (result) continue;
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
			continue;
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
					continue;
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
						continue;
					}
				}
			}
		}
		auto e = awst::makeAssignmentExpression(
			std::move(assignTarget), std::move(assignValue), m_loc);
		m_ctx.queuePostExpression(e, m_loc);
	}
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
