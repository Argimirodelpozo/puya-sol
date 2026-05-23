/// @file SolAssignmentTuple.cpp
/// Tuple-assignment translation extracted from SolAssignmentHandlers.cpp:
///   - handleTupleAssignment: destructuring `(a, b) = expr`
///   - buildTupleWithUpdatedField: copy-on-write for named-WTuple field writes
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

	// Side-effecting non-tuple RHS that returns a WTuple — typically a
	// tuple-returning function call: `(a, b) = f(...)` where `f` returns
	// `(uint, uint)`. Without snapshotting, each LHS element's
	// `TupleItemExpression(f(...), i)` carries a fresh `SubroutineCallExpression`
	// and puya re-emits the call once per element — multiplying side
	// effects and re-reading state AFTER prior calls' writes have already
	// committed. Cache the call result in a single temp var so each
	// TupleItem reads from the cached tuple. The TupleExpression-RHS
	// snapshot below handles the literal `(x, y) = (a(), b())` pattern;
	// this is its single-call analogue.
	if (dynamic_cast<awst::SubroutineCallExpression const*>(_value.get())
		|| dynamic_cast<awst::SubmitInnerTransaction const*>(_value.get()))
	{
		if (dynamic_cast<awst::WTuple const*>(_value->wtype))
		{
			static int s_callTupleCounter = 0;
			std::string tmpName = "__call_tuple_tmp_" + std::to_string(s_callTupleCounter++);
			awst::WType const* tupleWtype = _value->wtype;
			auto srcLoc = _value->sourceLocation;
			auto tmpVar = awst::makeVarExpression(tmpName, tupleWtype, srcLoc);
			auto tmpAssign = awst::makeAssignmentExpression(tmpVar, _value, srcLoc);
			m_ctx.prePendingStatements.push_back(
				awst::makeExpressionStatement(std::move(tmpAssign), srcLoc));
			_value = awst::makeVarExpression(tmpName, tupleWtype, srcLoc);
		}
	}

	// If the RHS is a literal tuple of VarExpressions (local variables),
	// snapshot each item into a temporary variable first so later element
	// reads see the pre-assignment value — otherwise `(a, b) = (b, a)` would
	// be evaluated as `a = b; b = a;` (lazy refs), breaking the swap.
	// We limit the snapshot to pure VarExpression items so that storage
	// variable tuples keep the (EVM-documented, intentionally broken)
	// in-place assignment semantics that tests like swap_in_storage_overwrite
	// rely on.
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
		// Also snapshot scalar RHS items when the LHS targets any transient
		// state variable: transient writes go through a subroutine (TSTORE
		// on a packed scratch blob), which can clobber other transient
		// state reads in the same tuple. Snapshotting RHS reads into temps
		// first preserves the pre-assignment values. Aggregate types keep
		// the in-place swap semantics Solidity documents for tuple swaps.
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
		// Side-effecting RHS + index-into-state-var LHS: when the tuple
		// has BOTH a side-effecting RHS item (e.g. `returnsArray()` that
		// reassigns a state variable) AND an LHS slot that indexes into
		// that same state variable (e.g. `arrayData[3]`), each LHS access
		// re-evaluates the underlying TupleItemExpression's base. The
		// repeated `returnsArray()` invocations clobber prior writes to
		// `arrayData[3]`. Snapshot every RHS slot to a temp once so each
		// LHS reads the committed value.
		//
		// The LHS-must-index-state guard avoids triggering on the
		// (y, y, y) = (set(1), set(2), set(3)) tuple-swap pattern, where
		// puya's optimizer + the snapshot interact badly: snapshot temps
		// get inlined back, the bare RHS tuple-of-reads is emitted as a
		// statement, and the original side-effecting calls leak stack
		// values that the redundant assignments don't consume.
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
					// IndexExpression(StateGet(BoxValueExpression),...) or
					// IndexExpression(AppStateExpression,...) — a write to
					// a state-array element is exactly the case where
					// re-evaluating the RHS clobbers prior writes.
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
		if (allLocalVars || allScalars || (hasSideEffectingRhs && lhsHasStateIndex))
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
				// Snapshot writes must run BEFORE the bare-RHS-tuple expression
				// (which the caller wraps in an ExpressionStatement) and
				// before any per-LHS assignment further down. `pendingStatements`
				// inserts AFTER the current statement, which would leave the
				// temps unassigned at the point the bare tuple reads them —
				// puya then DCEs the assignments and incorrectly leaves the
				// raw call return values on the stack. `prePendingStatements`
				// inserts BEFORE, so temps are committed before any read.
				m_ctx.prePendingStatements.push_back(std::move(stmt));

				auto tmpRead = awst::makeVarExpression(tmpName, rhsItem->wtype, _value->sourceLocation);
				newTuple->items.push_back(std::move(tmpRead));
				tmpTypes.push_back(rhsItem->wtype);
			}
			newTuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(
				std::move(tmpTypes), std::nullopt);
			_value = std::move(newTuple);
		}
	}

	// Collect assignments and insert in reverse order (right-to-left)
	// to match Solidity's tuple assignment semantics where later positions
	// are assigned first (important when the same target appears twice).
	std::vector<std::shared_ptr<awst::Statement>> assignStmts;
	auto pendingBefore = m_ctx.pendingStatements.size();

	for (size_t i = 0; i < items.size(); ++i)
	{
		auto item = items[i];

		// Skip null placeholders (empty-name VarExpression from tuple gaps like (,,a))
		if (auto const* varExpr = dynamic_cast<awst::VarExpression const*>(item.get()))
			if (varExpr->name.empty())
				continue;

		// Storage-pointer reassignment in tuple: `(m, v) = (m2, 21);` where
		// `m` is a `mapping storage`/`T[] storage` local. The AWST target
		// resolves to the current alias (e.g. BoxValueExpression for m1),
		// which is not a runtime lvalue for a pointer swap. Update the
		// compile-time alias and skip the slot's assignment stmt — mirrors
		// the simple `m = m2;` path above.
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
					// Prefer the RHS tuple's i-th AWST item directly: it
					// carries the underlying BoxValueExpression/AppStateExpression
					// needed for downstream mapping-key resolution. A
					// TupleItemExpression slice would lose that structure.
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
					// The slice may have come straight from the RHS tuple's
					// underlying state expression (StateGet/BoxValue/AppState),
					// or fallen back to a TupleItemExpression (TupleSlice).
					auto alias = wrappedStateRead
						|| dynamic_cast<awst::StateGet const*>(aliasExpr.get())
						? StorageAlias::stateRead(std::move(aliasExpr))
						: StorageAlias::tupleSlice(std::move(aliasExpr));
					m_scope.setStorageAlias(lhsDecl->id(), std::move(alias));
					continue;
				}
			}
		}

		// Use the VALUE tuple's element type (not the target's type)
		auto const* valueTuple = dynamic_cast<awst::WTuple const*>(_value->wtype);
		auto const* itemWtype = (valueTuple && i < valueTuple->types().size())
			? valueTuple->types()[i] : item->wtype;
		auto itemExpr = awst::makeTupleItem(_value, static_cast<int>(i), itemWtype, m_loc);

		auto assignTarget = item;
		if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(item.get()))
			assignTarget = decodeExpr->value;
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(assignTarget.get()))
			assignTarget = sg->field;

		std::shared_ptr<awst::Expression> assignValue = std::move(itemExpr);
		// Coerce string↔bytes via ReinterpretCast
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
			bool targetIsArc4 = false;
			switch (assignTarget->wtype->kind())
			{
			case awst::WTypeKind::ARC4UIntN:
			case awst::WTypeKind::ARC4StaticArray:
			case awst::WTypeKind::ARC4DynamicArray:
			case awst::WTypeKind::ARC4Struct:
				targetIsArc4 = true; break;
			default: break;
			}
			if (targetIsArc4)
			{
				assignValue = builder::TypeCoercion::stringToBytes(std::move(assignValue), m_loc);
				bool handled = false;
				// Array element-wise widening: arc4.{static,dynamic}_array<arc4.intM, ...>
				// → arc4.{static,dynamic}_array<arc4.intN, ...> (M < N). Pin source
				// bytes to a temp; the helper reads them multiple times.
				bool const sourceIsArc4Array =
					assignValue->wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| assignValue->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
				bool const targetIsArc4Array =
					assignTarget->wtype->kind() == awst::WTypeKind::ARC4StaticArray
					|| assignTarget->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
				if (sourceIsArc4Array && targetIsArc4Array)
				{
					static int s_widCounter = 0;
					std::string tmpName = "__widen_src_h_" + std::to_string(s_widCounter++);
					auto srcAsBytes = awst::makeAsBytes(assignValue, m_loc);
					auto tmpVar = awst::makeVarExpression(
						tmpName, awst::WType::bytesType(), m_loc);
					m_ctx.prePendingStatements.push_back(
						awst::makeAssignmentStatement(tmpVar, std::move(srcAsBytes), m_loc));
					auto const* sourceType = assignValue->wtype;
					auto mkSrc = [&]() {
						return awst::makeVarExpression(
							tmpName, awst::WType::bytesType(), m_loc);
					};
					std::shared_ptr<awst::Expression> widened;
					if (assignTarget->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
					{
						widened = builder::TypeCoercion::tryWidenArc4StaticArrayInt(
							sourceType, assignTarget->wtype, mkSrc, m_loc);
					}
					else
					{
						widened = builder::TypeCoercion::tryWidenArc4DynamicArrayInt(
							sourceType, assignTarget->wtype, mkSrc,
							[this](std::shared_ptr<awst::Statement> _s) {
								m_ctx.prePendingStatements.push_back(std::move(_s));
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
					if (auto narrowed = builder::TypeCoercion::tryNarrowUInt64ToArc4UIntN(
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

		// ARC4Struct field — copy-on-write (simplified: delegate to struct field handler below)
		if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(assignTarget.get()))
		{
			auto const* structType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
			if (!structType)
				if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
					structType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);

			if (structType)
			{
				auto result = handleStructFieldAssignment(fieldExpr, std::move(assignValue), assignTarget);
				if (result) continue;
			}
		}

		if (assignTarget->wtype != assignValue->wtype
			&& assignTarget->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
		{
			auto enc = awst::makeARC4Encode(std::move(assignValue), assignTarget->wtype, m_loc);
			assignValue = std::move(enc);
		}

		// Nested tuple destructuring: if the target is itself a TupleExpression,
		// recursively destructure instead of creating a direct assignment.
		if (dynamic_cast<awst::TupleExpression const*>(assignTarget.get()))
		{
			handleTupleAssignment(assignTarget, std::move(assignValue));
			continue;
		}

		// Transient state variable write: route through TransientStorage so
		// the assignment hits the scratch-slot packed blob rather than
		// producing an AssignmentExpression whose target is a ReinterpretCast
		// (which isn't an Lvalue in puya's AWST).
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
					auto stmt = m_ctx.transientStorage->buildWrite(srcIdent->name(), coerced, m_loc);
					if (stmt)
						m_ctx.pendingStatements.push_back(std::move(stmt));
					continue;
				}
			}
		}

		auto e = awst::makeAssignmentExpression(
			std::move(assignTarget), std::move(assignValue), m_loc);
		m_ctx.queueStmt(e, m_loc);
	}

	// Reverse the assignments added during this call to get right-to-left order.
	// This matches Solidity's viaYul semantics where tuple stores happen
	// right-to-left (last element stored first, first element stored last).
	auto pendingAfter = m_ctx.pendingStatements.size();
	if (pendingAfter > pendingBefore + 1)
		std::reverse(
			m_ctx.pendingStatements.begin() + static_cast<long>(pendingBefore),
			m_ctx.pendingStatements.end());

	return _value;
}

} // namespace puyasol::builder::sol_ast
