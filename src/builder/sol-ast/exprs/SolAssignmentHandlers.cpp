/// @file SolAssignmentHandlers.cpp
/// Shape-specific assignment handlers extracted from SolAssignment.cpp:
/// tuple destructuring, bytes-element writes, struct-field writes (storage
/// and memory), and the bytes-write helper. The dispatcher (toAwst) and the
/// copy-on-write tuple helper (buildTupleWithUpdatedField) remain in
/// SolAssignment.cpp.

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

std::shared_ptr<awst::Expression> SolAssignment::handleTupleAssignment(
	std::shared_ptr<awst::Expression> _target,
	std::shared_ptr<awst::Expression> _value,
	solidity::frontend::TupleExpression const* _sourceLhs)
{
	auto const* tupleTarget = dynamic_cast<awst::TupleExpression const*>(_target.get());
	auto const& items = tupleTarget->items;

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
					if (dynamic_cast<awst::BoxValueExpression const*>(base)
						|| dynamic_cast<awst::AppStateExpression const*>(base))
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
			auto newTuple = std::make_shared<awst::TupleExpression>();
			newTuple->sourceLocation = _value->sourceLocation;
			for (size_t i = 0; i < rhsTuple->items.size(); ++i)
			{
				auto const& rhsItem = rhsTuple->items[i];
				std::string tmpName = "__tuple_tmp_" + std::to_string(m_loc.line)
					+ "_" + std::to_string(i);

				auto tmpTarget = awst::makeVarExpression(tmpName, rhsItem->wtype, _value->sourceLocation);

				auto tmpAssign = std::make_shared<awst::AssignmentExpression>();
				tmpAssign->sourceLocation = _value->sourceLocation;
				tmpAssign->wtype = rhsItem->wtype;
				tmpAssign->target = tmpTarget;
				tmpAssign->value = rhsItem;

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
						auto slice = std::make_shared<awst::TupleItemExpression>();
						slice->sourceLocation = m_loc;
						slice->wtype = sliceType;
						slice->base = _value;
						slice->index = static_cast<int>(i);
						aliasExpr = slice;
					}
					if (dynamic_cast<awst::BoxValueExpression const*>(aliasExpr.get())
						|| dynamic_cast<awst::AppStateExpression const*>(aliasExpr.get()))
					{
						auto sg = std::make_shared<awst::StateGet>();
						sg->sourceLocation = m_loc;
						sg->wtype = aliasExpr->wtype;
						sg->field = aliasExpr;
						sg->defaultValue = StorageMapper::makeDefaultValue(aliasExpr->wtype, m_loc);
						aliasExpr = sg;
					}
					m_ctx.storageAliases[lhsDecl->id()] = std::move(aliasExpr);
					continue;
				}
			}
		}

		auto itemExpr = std::make_shared<awst::TupleItemExpression>();
		itemExpr->sourceLocation = m_loc;
		// Use the VALUE tuple's element type (not the target's type)
		auto const* valueTuple = dynamic_cast<awst::WTuple const*>(_value->wtype);
		if (valueTuple && i < valueTuple->types().size())
			itemExpr->wtype = valueTuple->types()[i];
		else
			itemExpr->wtype = item->wtype;
		itemExpr->base = _value;
		itemExpr->index = static_cast<int>(i);

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
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = m_loc;
				encode->wtype = assignTarget->wtype;
				encode->value = std::move(assignValue);
				assignValue = std::move(encode);
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
			auto enc = std::make_shared<awst::ARC4Encode>();
			enc->sourceLocation = m_loc;
			enc->wtype = assignTarget->wtype;
			enc->value = std::move(assignValue);
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

		auto e = std::make_shared<awst::AssignmentExpression>();
		e->sourceLocation = m_loc;
		e->wtype = assignTarget->wtype;
		e->target = std::move(assignTarget);
		e->value = std::move(assignValue);
		auto stmt = awst::makeExpressionStatement(e, m_loc);
		m_ctx.pendingStatements.push_back(std::move(stmt));
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

std::shared_ptr<awst::Expression> SolAssignment::handleBytesElementAssignment(
	awst::IndexExpression const* _indexExpr,
	std::shared_ptr<awst::Expression> _value)
{
	Token op = m_assignment.assignmentOperator();

	if (op != Token::Assign)
	{
		auto currentValue = buildExpr(m_assignment.leftHandSide());
		auto* solType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, op, solType, currentValue, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(op, std::move(currentValue), std::move(_value),
				_indexExpr->wtype, m_loc);
	}

	// Coerce value to single byte
	if (_value->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
		itob->stackArgs.push_back(std::move(_value));
		auto seven = awst::makeIntegerConstant("7", m_loc);
		auto one = awst::makeIntegerConstant("1", m_loc);
		auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), m_loc);
		extract->stackArgs.push_back(std::move(itob));
		extract->stackArgs.push_back(std::move(seven));
		extract->stackArgs.push_back(std::move(one));
		_value = std::move(extract);
	}
	else if (_value->wtype && _value->wtype->kind() == awst::WTypeKind::Bytes
		&& _value->wtype != awst::WType::bytesType())
	{
		auto cast = awst::makeReinterpretCast(std::move(_value), awst::WType::bytesType(), m_loc);
		_value = std::move(cast);
	}
	_value = builder::TypeCoercion::stringToBytes(std::move(_value), m_loc);

	auto replace = awst::makeIntrinsicCall("replace3", _indexExpr->base->wtype, m_loc);
	replace->stackArgs.push_back(_indexExpr->base);
	replace->stackArgs.push_back(_indexExpr->index);
	replace->stackArgs.push_back(std::move(_value));

	// AssignmentExpression.target must be an Lvalue (VarExpression,
	// FieldExpression, IndexExpression, TupleExpression, or a storage
	// expression). For `bytes(x)[i] = …` the IndexExpression base is a
	// ReinterpretCast wrapping the actual storage expression; unwrap it
	// so puya sees a plain lvalue, and adapt the target/value wtype to
	// match the underlying storage type (string ↔ bytes).
	auto target = _indexExpr->base;
	std::shared_ptr<awst::Expression> replaceValue = replace;
	while (auto const* cast = dynamic_cast<awst::ReinterpretCast const*>(target.get()))
		target = cast->expr;

	// Nested bytes field: `s.b[i] = v` where `s.b` is `bytes` but the struct
	// holds it as an ARC4 byte[]. The target here is ARC4Decode(FieldExpr)
	// which puya rejects as an lvalue — route through a NewStruct write-back.
	if (auto const* decode = dynamic_cast<awst::ARC4Decode const*>(target.get()))
	{
		if (auto const* fe = dynamic_cast<awst::FieldExpression const*>(decode->value.get()))
		{
			auto const* structType = dynamic_cast<awst::ARC4Struct const*>(fe->base->wtype);
			if (!structType)
				if (auto const* sg = dynamic_cast<awst::StateGet const*>(fe->base.get()))
					structType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
			if (structType)
				return buildStructFieldBytesWrite(fe, structType, std::move(replaceValue));
		}
	}

	if (target->wtype != replaceValue->wtype)
	{
		auto adaptCast = awst::makeReinterpretCast(std::move(replaceValue), target->wtype, m_loc);
		replaceValue = std::move(adaptCast);
	}

	auto e = std::make_shared<awst::AssignmentExpression>();
	e->sourceLocation = m_loc;
	e->wtype = target->wtype;
	e->target = target;
	e->value = std::move(replaceValue);
	return e;
}

std::shared_ptr<awst::Expression> SolAssignment::buildStructFieldBytesWrite(
	awst::FieldExpression const* _fieldExpr,
	awst::ARC4Struct const* _structType,
	std::shared_ptr<awst::Expression> _newBytes)
{
	auto base = _fieldExpr->base;
	std::string fieldName = _fieldExpr->name;

	// Unwrap StateGet for write targets (StateGet is not an Lvalue)
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
		base = sg->field;

	// Wrap bytes → ARC4 byte[] encoding (prepends length prefix in puya)
	awst::WType const* arc4FieldType = nullptr;
	for (auto const& [fname, ftype]: _structType->fields())
		if (fname == fieldName) { arc4FieldType = ftype; break; }

	std::shared_ptr<awst::Expression> encodedValue = std::move(_newBytes);
	if (arc4FieldType && encodedValue->wtype != arc4FieldType)
	{
		auto encode = std::make_shared<awst::ARC4Encode>();
		encode->sourceLocation = m_loc;
		encode->wtype = arc4FieldType;
		encode->value = std::move(encodedValue);
		encodedValue = std::move(encode);
	}

	// Build NewStruct with replaced field
	auto newStruct = std::make_shared<awst::NewStruct>();
	newStruct->sourceLocation = m_loc;
	newStruct->wtype = _structType;
	for (auto const& [fname, ftype]: _structType->fields())
	{
		if (fname == fieldName)
			newStruct->values[fname] = encodedValue;
		else
		{
			auto f = std::make_shared<awst::FieldExpression>();
			f->sourceLocation = m_loc;
			f->base = base;
			f->name = fname;
			f->wtype = ftype;
			newStruct->values[fname] = std::move(f);
		}
	}

	// Walk outer FieldExpression chain, rebuilding NewStructs (copy-on-write
	// for nested `outer.inner.b[i] = v` patterns).
	auto assignTarget = base;
	std::shared_ptr<awst::Expression> assignValue = std::move(newStruct);

	while (auto const* outerField = dynamic_cast<awst::FieldExpression const*>(assignTarget.get()))
	{
		auto const* outerStructType = dynamic_cast<awst::ARC4Struct const*>(outerField->base->wtype);
		if (!outerStructType)
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(outerField->base.get()))
				outerStructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
		if (!outerStructType) break;

		auto outerBase = outerField->base;
		auto outerWriteBase = outerBase;
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(outerBase.get()))
			outerWriteBase = sg->field;
		auto outerReadBase = outerBase;
		if (dynamic_cast<awst::BoxValueExpression const*>(outerWriteBase.get())
			&& !dynamic_cast<awst::StateGet const*>(outerBase.get()))
		{
			auto sg = std::make_shared<awst::StateGet>();
			sg->sourceLocation = m_loc;
			sg->wtype = outerWriteBase->wtype;
			sg->field = outerWriteBase;
			sg->defaultValue = builder::StorageMapper::makeDefaultValue(outerWriteBase->wtype, m_loc);
			outerReadBase = sg;
		}

		std::string outerFieldName = outerField->name;

		auto outerNewStruct = std::make_shared<awst::NewStruct>();
		outerNewStruct->sourceLocation = m_loc;
		outerNewStruct->wtype = outerStructType;
		for (auto const& [fn, ft]: outerStructType->fields())
		{
			if (fn == outerFieldName)
				outerNewStruct->values[fn] = std::move(assignValue);
			else
			{
				auto f = std::make_shared<awst::FieldExpression>();
				f->sourceLocation = m_loc;
				f->base = outerReadBase;
				f->name = fn;
				f->wtype = ft;
				outerNewStruct->values[fn] = std::move(f);
			}
		}
		assignTarget = std::move(outerWriteBase);
		assignValue = std::move(outerNewStruct);
	}

	// Unwrap StateGet nested inside an IndexExpression lvalue (puya rejects).
	if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(assignTarget.get()))
	{
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(idx->base.get()))
		{
			auto newIdx = std::make_shared<awst::IndexExpression>();
			newIdx->sourceLocation = idx->sourceLocation;
			newIdx->wtype = idx->wtype;
			newIdx->base = sg->field;
			newIdx->index = idx->index;
			assignTarget = std::move(newIdx);
		}
	}

	auto e = std::make_shared<awst::AssignmentExpression>();
	e->sourceLocation = m_loc;
	e->wtype = assignTarget->wtype;
	e->target = std::move(assignTarget);
	e->value = std::move(assignValue);
	return e;
}

std::shared_ptr<awst::Expression> SolAssignment::handleStructFieldAssignment(
	awst::FieldExpression const* _fieldExpr,
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _unwrappedTarget)
{
	auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(_fieldExpr->base->wtype);
	if (!arc4StructType)
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(_fieldExpr->base.get()))
			arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
	if (!arc4StructType) return nullptr;

	Token op = m_assignment.assignmentOperator();
	auto base = _fieldExpr->base;
	std::string fieldName = _fieldExpr->name;

	if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
		base = sg->field;

	auto readBase = base;
	if (dynamic_cast<awst::BoxValueExpression const*>(base.get()))
	{
		auto stateGet = std::make_shared<awst::StateGet>();
		stateGet->sourceLocation = m_loc;
		stateGet->wtype = base->wtype;
		stateGet->field = base;
		stateGet->defaultValue = builder::StorageMapper::makeDefaultValue(base->wtype, m_loc);
		readBase = stateGet;
	}

	if (op != Token::Assign)
	{
		auto currentField = std::make_shared<awst::FieldExpression>();
		currentField->sourceLocation = m_loc;
		currentField->base = readBase;
		currentField->name = fieldName;
		currentField->wtype = _fieldExpr->wtype;
		auto decoded = std::make_shared<awst::ARC4Decode>();
		decoded->sourceLocation = m_loc;
		decoded->wtype = m_ctx.typeMapper.map(m_assignment.leftHandSide().annotation().type);
		decoded->value = std::move(currentField);
		auto* solType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, op, solType, decoded, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(op, std::move(decoded), std::move(_value),
				decoded->wtype, m_loc);
	}

	// ARC4Encode the value
	awst::WType const* arc4FieldType = nullptr;
	for (auto const& [fname, ftype]: arc4StructType->fields())
		if (fname == fieldName) { arc4FieldType = ftype; break; }
	if (arc4FieldType && _value->wtype != arc4FieldType)
	{
		// Coerce value to native type before ARC4 encoding
		// e.g., IntegerConstant(uint64, "2") → BytesConstant(bytes[1]) for bytes1 fields
		auto* nativeType = m_ctx.typeMapper.map(m_assignment.leftHandSide().annotation().type);
		if (nativeType && _value->wtype != nativeType)
			_value = builder::TypeCoercion::coerceForAssignment(std::move(_value), nativeType, m_loc);

		auto encode = std::make_shared<awst::ARC4Encode>();
		encode->sourceLocation = m_loc;
		encode->wtype = arc4FieldType;
		encode->value = std::move(_value);
		_value = std::move(encode);
	}

	// Build NewStruct with copy-on-write
	auto newStruct = std::make_shared<awst::NewStruct>();
	newStruct->sourceLocation = m_loc;
	newStruct->wtype = arc4StructType;
	for (auto const& [fname, ftype]: arc4StructType->fields())
	{
		if (fname == fieldName)
			newStruct->values[fname] = std::move(_value);
		else
		{
			auto field = std::make_shared<awst::FieldExpression>();
			field->sourceLocation = m_loc;
			field->base = readBase;
			field->name = fname;
			field->wtype = ftype;
			newStruct->values[fname] = std::move(field);
		}
	}

	// Recursive copy-on-write for nested structs
	auto assignTarget2 = std::move(base);
	std::shared_ptr<awst::Expression> assignValue2 = std::move(newStruct);
	std::vector<std::pair<std::string, awst::WType const*>> fieldChain;

	while (auto const* outerField = dynamic_cast<awst::FieldExpression const*>(assignTarget2.get()))
	{
		auto const* outerStructType = dynamic_cast<awst::ARC4Struct const*>(outerField->base->wtype);
		if (!outerStructType) break;
		auto outerBase = outerField->base;
		// Unwrap StateGet for assignment targets (StateGet is not an Lvalue)
		auto outerWriteBase = outerBase;
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(outerBase.get()))
			outerWriteBase = sg->field;
		// Wrap in StateGet for reads if needed
		auto outerReadBase = outerBase;
		if (dynamic_cast<awst::BoxValueExpression const*>(outerWriteBase.get())
			&& !dynamic_cast<awst::StateGet const*>(outerBase.get()))
		{
			auto sg = std::make_shared<awst::StateGet>();
			sg->sourceLocation = m_loc;
			sg->wtype = outerWriteBase->wtype;
			sg->field = outerWriteBase;
			sg->defaultValue = builder::StorageMapper::makeDefaultValue(outerWriteBase->wtype, m_loc);
			outerReadBase = sg;
		}

		std::string outerFieldName = outerField->name;
		awst::WType const* outerFieldWtype = nullptr;
		for (auto const& [fn, ft]: outerStructType->fields())
			if (fn == outerFieldName) { outerFieldWtype = ft; break; }
		fieldChain.push_back({outerFieldName, outerFieldWtype});

		auto outerNewStruct = std::make_shared<awst::NewStruct>();
		outerNewStruct->sourceLocation = m_loc;
		outerNewStruct->wtype = outerStructType;
		for (auto const& [fn, ft]: outerStructType->fields())
		{
			if (fn == outerFieldName)
				outerNewStruct->values[fn] = std::move(assignValue2);
			else
			{
				auto f = std::make_shared<awst::FieldExpression>();
				f->sourceLocation = m_loc;
				f->base = outerReadBase; // Use StateGet-wrapped for reads
				f->name = fn;
				f->wtype = ft;
				outerNewStruct->values[fn] = std::move(f);
			}
		}
		assignTarget2 = std::move(outerWriteBase); // Use unwrapped for target
		assignValue2 = std::move(outerNewStruct);
	}

	// If the outermost write target is an IndexExpression whose base is
	// wrapped in StateGet (e.g. `data[2].x = v` on a box-stored static
	// array struct), unwrap the StateGet so the assignment target carries
	// a raw BoxValue — puya rejects StateGet nested inside an Lvalue.
	if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(assignTarget2.get()))
	{
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(idx->base.get()))
		{
			auto newIdx = std::make_shared<awst::IndexExpression>();
			newIdx->sourceLocation = idx->sourceLocation;
			newIdx->wtype = idx->wtype;
			newIdx->base = sg->field;
			newIdx->index = idx->index;
			assignTarget2 = std::move(newIdx);
		}
	}

	// Mapping-entry partial write: `n[k][i].f = v` where n is
	// `mapping(K => T[N])` lowers to box_replace on the per-entry key.
	// The per-entry box is created lazily. Emit box_create as a pending
	// pre-statement so the box exists before box_replace runs.
	if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(assignTarget2.get()))
	{
		if (auto const* bv = dynamic_cast<awst::BoxValueExpression const*>(idx->base.get()))
		{
			if (bv->key && dynamic_cast<awst::BoxPrefixedKeyExpression const*>(bv->key.get()))
			{
				uint64_t totalSize = 0;
				if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(bv->wtype))
				{
					int elemSize = builder::TypeCoercion::computeEncodedElementSize(sa->elementType());
					if (elemSize > 0 && sa->arraySize() > 0)
						totalSize = static_cast<uint64_t>(elemSize) * static_cast<uint64_t>(sa->arraySize());
				}
				if (totalSize > 0 && totalSize <= 32768)
				{
					auto createCall = awst::makeIntrinsicCall(
						"box_create", awst::WType::boolType(), m_loc);
					createCall->stackArgs.push_back(bv->key);
					createCall->stackArgs.push_back(
						awst::makeIntegerConstant(std::to_string(totalSize), m_loc));
					auto createStmt = awst::makeExpressionStatement(std::move(createCall), m_loc);
					m_ctx.prePendingStatements.push_back(std::move(createStmt));
				}
			}
		}
	}

	auto e = std::make_shared<awst::AssignmentExpression>();
	e->sourceLocation = m_loc;
	e->wtype = assignTarget2->wtype;
	e->target = std::move(assignTarget2);
	e->value = std::move(assignValue2);

	if (arc4FieldType)
	{
		std::shared_ptr<awst::Expression> extractBase = std::move(e);
		for (auto it = fieldChain.rbegin(); it != fieldChain.rend(); ++it)
		{
			auto fe = std::make_shared<awst::FieldExpression>();
			fe->sourceLocation = m_loc;
			fe->base = std::move(extractBase);
			fe->name = it->first;
			fe->wtype = it->second;
			extractBase = std::move(fe);
		}
		auto fieldExtract = std::make_shared<awst::FieldExpression>();
		fieldExtract->sourceLocation = m_loc;
		fieldExtract->base = std::move(extractBase);
		fieldExtract->name = fieldName;
		fieldExtract->wtype = arc4FieldType;
		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = m_loc;
		decode->wtype = m_ctx.typeMapper.map(m_assignment.annotation().type);
		decode->value = std::move(fieldExtract);
		return decode;
	}
	return e;
}


} // namespace puyasol::builder::sol_ast
