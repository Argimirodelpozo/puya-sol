/// @file SolAssignmentTuple.cpp — handleTupleAssignment + buildTupleWithUpdatedField
#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4ArrayWidening.h"
#include "builder/sol-types/TypeCoercion.h"

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
				// Must use prePendingStatements (not pendingStatements): pending
				// inserts AFTER the current statement, leaving temps unassigned
				// when the bare tuple reads them — puya DCEs the assignments and
				// leaks raw call return values on the stack.
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

	// Collect then reverse (right-to-left) to match Solidity viaYul tuple semantics.
	std::vector<std::shared_ptr<awst::Statement>> assignStmts;
	auto pendingBefore = m_ctx.pendingStatements.size();

	for (size_t i = 0; i < items.size(); ++i)
	{
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
					m_scope.setStorageAlias(lhsDecl->id(), std::move(alias));
					continue;
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
						widened = builder::tryWidenArc4StaticArrayInt(
							sourceType, assignTarget->wtype, mkSrc, m_loc);
					}
					else
					{
						widened = builder::tryWidenArc4DynamicArrayInt(
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

	// Reverse to right-to-left (Solidity viaYul: last element stored first).
	auto pendingAfter = m_ctx.pendingStatements.size();
	if (pendingAfter > pendingBefore + 1)
		std::reverse(
			m_ctx.pendingStatements.begin() + static_cast<long>(pendingBefore),
			m_ctx.pendingStatements.end());

	return _value;
}

} // namespace puyasol::builder::sol_ast
