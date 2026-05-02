/// @file SolAssignment.cpp
/// Migrated from AssignmentBuilder.cpp.

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

SolAssignment::SolAssignment(eb::BuilderContext& _ctx, Assignment const& _node)
	: SolExpression(_ctx, _node), m_assignment(_node)
{
}

std::shared_ptr<awst::Expression> SolAssignment::buildTupleWithUpdatedField(
	std::shared_ptr<awst::Expression> _base,
	std::string const& _fieldName,
	std::shared_ptr<awst::Expression> _newValue)
{
	auto const* tupleType = dynamic_cast<awst::WTuple const*>(_base->wtype);
	auto const& names = *tupleType->names();
	auto const& types = tupleType->types();

	auto tuple = std::make_shared<awst::TupleExpression>();
	tuple->sourceLocation = m_loc;
	tuple->wtype = _base->wtype;

	for (size_t i = 0; i < names.size(); ++i)
	{
		if (names[i] == _fieldName)
			tuple->items.push_back(std::move(_newValue));
		else
		{
			auto field = std::make_shared<awst::FieldExpression>();
			field->sourceLocation = m_loc;
			field->base = _base;
			field->name = names[i];
			field->wtype = types[i];
			tuple->items.push_back(std::move(field));
		}
	}
	return tuple;
}

std::shared_ptr<awst::Expression> SolAssignment::toAwst()
{
	Token op = m_assignment.assignmentOperator();

	// Transient state var write: intercept before the generic path so the
	// write goes through TransientStorage (scratch slot TRANSIENT_SLOT
	// packed blob) rather than through the app_global storage mapper.
	// Covers simple `x = v` and compound `x += v` forms for identifier LHS.
	if (auto const* lhsIdent = dynamic_cast<Identifier const*>(&m_assignment.leftHandSide()))
	{
		auto const* lhsDecl = dynamic_cast<VariableDeclaration const*>(
			lhsIdent->annotation().referencedDeclaration);
		if (lhsDecl && lhsDecl->isStateVariable()
			&& lhsDecl->referenceLocation() == VariableDeclaration::Location::Transient
			&& m_ctx.transientStorage
			&& m_ctx.transientStorage->isTransient(*lhsDecl))
		{
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
	}

	// Storage pointer reassignment: `mapping storage m = m1; ...; m = m2;`
	// The LHS is a local with Storage reference location. There is no
	// runtime write — just update the compile-time alias so that later
	// `m[k]` accesses resolve to the new state variable. The new alias
	// is the *same* expression we already built for the RHS, wrapped in
	// StateGet if necessary so subsequent reads through the alias still
	// return a value.
	if (op == Token::Assign)
	{
		if (auto const* lhsIdent = dynamic_cast<Identifier const*>(&m_assignment.leftHandSide()))
		{
			auto const* lhsDecl = dynamic_cast<VariableDeclaration const*>(
				lhsIdent->annotation().referencedDeclaration);
			if (lhsDecl
				&& lhsDecl->referenceLocation() == VariableDeclaration::Location::Storage
				&& !lhsDecl->isStateVariable())
			{
				// Mapping-key-param locals (`mapping(K=>V) storage r` returned
				// from a function or declared inside one) hold the box-key
				// prefix as a runtime bytes value. Reassigning them must do
				// an actual bytes write; the compile-time alias path drops
				// the side effect (returns VoidConstant) which is fine for
				// state-var aliases but loses runtime mutations like
				// `r = a; r[k] = v; r = b; r[k] = v;`.
				if (m_ctx.mappingKeyParams.count(lhsDecl->id()))
				{
					auto rhsExpr = buildExpr(m_assignment.rightHandSide());
					if (rhsExpr->wtype != awst::WType::bytesType())
					{
						rhsExpr = builder::TypeCoercion::coerceForAssignment(
							std::move(rhsExpr), awst::WType::bytesType(), m_loc);
					}
					auto var = awst::makeVarExpression(
						lhsIdent->name(), awst::WType::bytesType(), m_loc);
					auto e = std::make_shared<awst::AssignmentExpression>();
					e->sourceLocation = m_loc;
					e->wtype = awst::WType::bytesType();
					e->target = std::move(var);
					e->value = std::move(rhsExpr);
					return e;
				}

				auto rhsExpr = buildExpr(m_assignment.rightHandSide());
				auto aliasExpr = rhsExpr;
				if (dynamic_cast<awst::BoxValueExpression const*>(rhsExpr.get())
					|| dynamic_cast<awst::AppStateExpression const*>(rhsExpr.get()))
				{
					auto sg = std::make_shared<awst::StateGet>();
					sg->sourceLocation = m_loc;
					sg->wtype = rhsExpr->wtype;
					sg->field = rhsExpr;
					sg->defaultValue = StorageMapper::makeDefaultValue(rhsExpr->wtype, m_loc);
					aliasExpr = sg;
				}
				m_ctx.storageAliases[lhsDecl->id()] = std::move(aliasExpr);
				auto voidExpr = std::make_shared<awst::VoidConstant>();
				voidExpr->sourceLocation = m_loc;
				voidExpr->wtype = awst::WType::voidType();
				return voidExpr;
			}
		}
	}

	// Rewrite `arr.push() = value` as `arr.push(value)`. Solidity's
	// arg-less push() returns a reference to the new slot; we don't
	// have a reference type, so stash the RHS as a pending "push
	// value" in the build context before translating the LHS.
	// SolArrayMethod::push() picks it up and uses it as the new
	// element instead of emitting a default-valued push as a pending
	// statement and returning VoidConstant.
	bool pushAssignRewrite = false;
	if (op == Token::Assign)
	{
		if (auto const* lhsCall = dynamic_cast<FunctionCall const*>(&m_assignment.leftHandSide()))
		{
			if (lhsCall->arguments().empty())
			{
				if (auto const* member = dynamic_cast<MemberAccess const*>(&lhsCall->expression()))
				{
					if (member->memberName() == "push")
					{
						m_ctx.pendingArrayPushValue = buildExpr(m_assignment.rightHandSide());
						pushAssignRewrite = true;
					}
				}
			}
		}
	}

	auto target = buildExpr(m_assignment.leftHandSide());
	if (pushAssignRewrite)
	{
		m_ctx.pendingArrayPushValue.reset();
		// `target` is now the ArrayExtend expression emitted by
		// SolArrayMethod. Return it directly as the assignment
		// result — the assignment's effect is already encoded in the
		// ArrayExtend.
		return target;
	}
	auto value = buildExpr(m_assignment.rightHandSide());

	// Enum range validation: EVM panics (0x21) on assigning invalid enum values
	if (op == Token::Assign)
	{
		auto const* lhsType = m_assignment.leftHandSide().annotation().type;
		if (auto const* enumType = dynamic_cast<EnumType const*>(lhsType))
		{
			unsigned numMembers = enumType->numberOfMembers();
			auto val = builder::TypeCoercion::implicitNumericCast(value, awst::WType::uint64Type(), m_loc);

			auto maxVal = awst::makeIntegerConstant(std::to_string(numMembers), m_loc);

			auto cmp = awst::makeNumericCompare(val, awst::NumericComparison::Lt, std::move(maxVal), m_loc);

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), m_loc, "enum out of range"), m_loc);
			m_ctx.prePendingStatements.push_back(std::move(assertStmt));

			value = std::move(val);
		}
	}

	// Slot-based storage write: target is biguint (slot offset), value is array
	// Expand to individual __storage_write(slot + j, value[j]) calls
	if (op == Token::Assign && target->wtype == awst::WType::biguintType())
	{
		auto const* lhsType = m_assignment.leftHandSide().annotation().type;
		auto const* arrType = lhsType ? dynamic_cast<ArrayType const*>(lhsType) : nullptr;
		if (!arrType)
		{
			// Check if RHS is an array type
			auto const* rhsType = m_assignment.rightHandSide().annotation().type;
			arrType = rhsType ? dynamic_cast<ArrayType const*>(rhsType) : nullptr;
		}
		if (arrType && !arrType->isDynamicallySized())
		{
			unsigned len = static_cast<unsigned>(arrType->length());
			// Emit: for j in 0..len-1: __storage_write(btoi(slot + j), value[j])
			for (unsigned j = 0; j < len; ++j)
			{
				// slot + j
				auto jConst = awst::makeIntegerConstant(std::to_string(j), m_loc, awst::WType::biguintType());

				auto slotJ = awst::makeBigUIntBinOp(target, awst::BigUIntBinaryOperator::Add, std::move(jConst), m_loc);

				// btoi(slot + j) for __storage_write
				auto castBytes = awst::makeReinterpretCast(std::move(slotJ), awst::WType::bytesType(), m_loc);

				// Safe truncate biguint slot to uint64: extract last 8 bytes then btoi
				auto lenOp = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
				lenOp->stackArgs.push_back(castBytes);
				auto sub8 = std::make_shared<awst::UInt64BinaryOperation>();
				sub8->sourceLocation = m_loc;
				sub8->wtype = awst::WType::uint64Type();
				sub8->left = std::move(lenOp);
				sub8->op = awst::UInt64BinaryOperator::Sub;
				auto eight = awst::makeIntegerConstant("8", m_loc);
				sub8->right = std::move(eight);
				auto last8 = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), m_loc);
				last8->stackArgs.push_back(std::move(castBytes));
				last8->stackArgs.push_back(std::move(sub8));
				auto eight2 = awst::makeIntegerConstant("8", m_loc);
				last8->stackArgs.push_back(std::move(eight2));
				auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), m_loc);
				btoi->stackArgs.push_back(std::move(last8));

				// value[j]
				auto idx = awst::makeIntegerConstant(std::to_string(j), m_loc);

				auto elemExpr = std::make_shared<awst::IndexExpression>();
				elemExpr->sourceLocation = m_loc;
				elemExpr->base = value; // shared
				elemExpr->index = std::move(idx);
				// Element type: use the ARC4 element type from the value's wtype
				if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(value->wtype))
					elemExpr->wtype = sa->elementType();
				else if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(value->wtype))
					elemExpr->wtype = da->elementType();
				else
					elemExpr->wtype = m_ctx.typeMapper.map(arrType->baseType());

				// If element is ARC4-encoded, decode to biguint
				std::shared_ptr<awst::Expression> elemVal = std::move(elemExpr);
				if (elemVal->wtype && elemVal->wtype->kind() == awst::WTypeKind::ARC4UIntN)
				{
					auto decode = std::make_shared<awst::ARC4Decode>();
					decode->sourceLocation = m_loc;
					decode->wtype = awst::WType::biguintType();
					decode->value = std::move(elemVal);
					elemVal = std::move(decode);
				}
				else if (elemVal->wtype == awst::WType::uint64Type())
				{
					// Convert uint64 to biguint
					auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
					itob->stackArgs.push_back(std::move(elemVal));
					auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
					elemVal = std::move(cast);
				}

				// __storage_write(slot, value)
				auto call = std::make_shared<awst::SubroutineCallExpression>();
				call->sourceLocation = m_loc;
				call->wtype = awst::WType::voidType();
				call->target = awst::InstanceMethodTarget{"__storage_write"};
				{
					awst::CallArg slotArg;
					slotArg.name = "__slot";
					slotArg.value = std::move(btoi);
					call->args.push_back(std::move(slotArg));

					awst::CallArg valArg;
					valArg.name = "__value";
					valArg.value = std::move(elemVal);
					call->args.push_back(std::move(valArg));
				}

				auto stmt = awst::makeExpressionStatement(std::move(call), m_loc);
				m_ctx.pendingStatements.push_back(std::move(stmt));
			}
			// Return a dummy value (void-like, the writes are in pending)
			auto zero = awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType());
			return zero;
		}
	}

	// Scalar slot-based storage write: target is a computed biguint slot (not an array assignment).
	// Emit __storage_write(btoi(slot), value) directly.
	if (dynamic_cast<awst::BigUIntBinaryOperation const*>(target.get())
		&& target->wtype == awst::WType::biguintType())
	{
		// Compound assignment: read current value first
		if (op != Token::Assign)
		{
			// __storage_read(btoi(slot))
			auto readSlot = builder::StorageMapper::biguintSlotToBtoi(target, m_loc);
			auto readCall = std::make_shared<awst::SubroutineCallExpression>();
			readCall->sourceLocation = m_loc;
			readCall->wtype = awst::WType::biguintType();
			readCall->target = awst::InstanceMethodTarget{"__storage_read"};
			awst::CallArg readArg;
			readArg.name = "__slot";
			readArg.value = std::move(readSlot);
			readCall->args.push_back(std::move(readArg));

			auto* targetSolType = m_assignment.leftHandSide().annotation().type;
			auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
				m_ctx, op, targetSolType, readCall, value, m_loc);
			if (builderResult)
				value = std::move(builderResult);
			else
				value = m_ctx.buildBinaryOp(op, std::move(readCall), std::move(value),
					target->wtype, m_loc);
		}

		auto btoi = builder::StorageMapper::biguintSlotToBtoi(target, m_loc);

		auto call = std::make_shared<awst::SubroutineCallExpression>();
		call->sourceLocation = m_loc;
		call->wtype = awst::WType::voidType();
		call->target = awst::InstanceMethodTarget{"__storage_write"};
		{
			awst::CallArg slotArg;
			slotArg.name = "__slot";
			slotArg.value = std::move(btoi);
			call->args.push_back(std::move(slotArg));

			awst::CallArg valArg;
			valArg.name = "__value";
			valArg.value = std::move(value);
			call->args.push_back(std::move(valArg));
		}

		auto stmt = awst::makeExpressionStatement(std::move(call), m_loc);
		m_ctx.pendingStatements.push_back(std::move(stmt));

		auto zero = awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType());
		return zero;
	}

	// Tuple decomposition
	if (dynamic_cast<awst::TupleExpression const*>(target.get()))
	{
		auto const* sourceLhs = dynamic_cast<solidity::frontend::TupleExpression const*>(
			&m_assignment.leftHandSide());
		return handleTupleAssignment(std::move(target), std::move(value), sourceLhs);
	}

	// Bytes element assignment: bytes[i] = value
	if (auto const* indexExpr = dynamic_cast<awst::IndexExpression const*>(target.get()))
	{
		if (indexExpr->base && indexExpr->base->wtype
			&& indexExpr->base->wtype->kind() == awst::WTypeKind::Bytes)
			return handleBytesElementAssignment(indexExpr, std::move(value));
	}

	// Unwrap ARC4Decode for Lvalue
	auto unwrappedTarget = target;
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		unwrappedTarget = decodeExpr->value;

	// Struct field copy-on-write
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(unwrappedTarget.get()))
	{
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (!arc4StructType)
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
				arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
		if (arc4StructType)
			return handleStructFieldAssignment(fieldExpr, std::move(value), unwrappedTarget);

		// WTuple named field assignment
		auto const* tupleType = dynamic_cast<awst::WTuple const*>(fieldExpr->base->wtype);
		if (tupleType && tupleType->names().has_value())
		{
			auto base = fieldExpr->base;
			std::string fieldName = fieldExpr->name;

			if (op != Token::Assign)
			{
				auto currentField = std::make_shared<awst::FieldExpression>();
				currentField->sourceLocation = m_loc;
				currentField->base = base;
				currentField->name = fieldName;
				currentField->wtype = fieldExpr->wtype;
				auto* solType = m_assignment.leftHandSide().annotation().type;
				auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
					m_ctx, op, solType, currentField, value, m_loc);
				if (builderResult)
					value = std::move(builderResult);
				else
					value = m_ctx.buildBinaryOp(op, std::move(currentField), std::move(value),
						fieldExpr->wtype, m_loc);
			}

			value = builder::TypeCoercion::implicitNumericCast(
				std::move(value), fieldExpr->wtype, m_loc);
			auto newTuple = buildTupleWithUpdatedField(base, fieldName, std::move(value));

			auto writeTarget = base;
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
				writeTarget = sg->field;

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = m_loc;
			e->wtype = writeTarget->wtype;
			e->target = std::move(writeTarget);
			e->value = std::move(newTuple);

			auto fieldExtract = std::make_shared<awst::FieldExpression>();
			fieldExtract->sourceLocation = m_loc;
			fieldExtract->base = std::move(e);
			fieldExtract->name = fieldName;
			fieldExtract->wtype = fieldExpr->wtype;
			return fieldExtract;
		}
	}

	// Compound assignment
	if (op != Token::Assign)
	{
		auto currentValue = buildExpr(m_assignment.leftHandSide());
		if (dynamic_cast<awst::BoxValueExpression const*>(currentValue.get()))
		{
			auto defaultVal = builder::StorageMapper::makeDefaultValue(currentValue->wtype, m_loc);
			auto stateGet = std::make_shared<awst::StateGet>();
			stateGet->sourceLocation = m_loc;
			stateGet->wtype = currentValue->wtype;
			stateGet->field = currentValue;
			stateGet->defaultValue = defaultVal;
			currentValue = std::move(stateGet);
		}
		auto* targetSolType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, op, targetSolType, currentValue, value, m_loc);
		if (builderResult)
			value = std::move(builderResult);
		else
			value = m_ctx.buildBinaryOp(op, std::move(currentValue), std::move(value),
				target->wtype, m_loc);
	}

	// Type coercion (handles int→bytes[N], string→bytes, numeric casts)
	value = builder::TypeCoercion::coerceForAssignment(std::move(value), target->wtype, m_loc);
	if (value->wtype != target->wtype && target->wtype
		&& target->wtype->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesType = dynamic_cast<awst::BytesWType const*>(target->wtype);
		auto const* strConst = dynamic_cast<awst::StringConstant const*>(value.get());
		if (bytesType && bytesType->length().has_value() && *bytesType->length() > 0 && strConst)
		{
			if (auto padded = builder::TypeCoercion::stringToBytesN(
					value.get(), target->wtype, *bytesType->length(), m_loc))
				value = std::move(padded);
		}
		else
		{
			bool valueIsCompatible = value->wtype == awst::WType::stringType()
				|| (value->wtype && value->wtype->kind() == awst::WTypeKind::Bytes);
			if (valueIsCompatible)
			{
				auto cast = awst::makeReinterpretCast(std::move(value), target->wtype, m_loc);
				value = std::move(cast);
			}
		}
	}
	if (value->wtype != target->wtype && target->wtype == awst::WType::stringType()
		&& value->wtype && (value->wtype->kind() == awst::WTypeKind::Bytes
			|| value->wtype == awst::WType::bytesType()))
	{
		auto cast = awst::makeReinterpretCast(std::move(value), target->wtype, m_loc);
		value = std::move(cast);
	}

	// Unwrap for assignment target
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		target = decodeExpr->value;
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
		target = sg->field;
	// Recursively unwrap StateGet from nested base chains
	// (e.g. data[i] or data[i].x where data is box-backed state)
	{
		std::shared_ptr<awst::Expression>* basePtr = nullptr;
		if (auto* indexExpr = dynamic_cast<awst::IndexExpression*>(target.get()))
			basePtr = &indexExpr->base;
		else if (auto* fieldExpr = dynamic_cast<awst::FieldExpression*>(target.get()))
			basePtr = &fieldExpr->base;
		while (basePtr)
		{
			if (auto const* baseSG = dynamic_cast<awst::StateGet const*>(basePtr->get()))
			{
				*basePtr = baseSG->field;
				break;
			}
			if (auto const* baseDecode = dynamic_cast<awst::ARC4Decode const*>(basePtr->get()))
			{
				*basePtr = baseDecode->value;
				// Continue checking the unwrapped expression
			}
			if (auto* innerIndex = dynamic_cast<awst::IndexExpression*>(basePtr->get()))
				basePtr = &innerIndex->base;
			else if (auto* innerField = dynamic_cast<awst::FieldExpression*>(basePtr->get()))
				basePtr = &innerField->base;
			else
				break;
		}
	}

	// ARC4 encode if needed
	if (value->wtype != target->wtype)
	{
		bool targetIsArc4 = false;
		switch (target->wtype->kind())
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
			// If value is already ARC4 with structurally matching type (pointers
			// differ only because TypeMapper didn't intern), skip the redundant
			// encode — it would otherwise double-encode an ARC4 aggregate.
			bool sameShape = value->wtype->kind() == target->wtype->kind()
				&& value->wtype->name() == target->wtype->name();
			if (!sameShape)
			{
				value = builder::TypeCoercion::stringToBytes(std::move(value), m_loc);
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = m_loc;
				encode->wtype = target->wtype;
				encode->value = std::move(value);
				value = std::move(encode);
			}
		}
	}

	// Mapping-entry partial write: `m[k][i] = v` where m is `mapping(K => T[N])`
	// or `mapping(K => bytes[N])`. The outer IndexExpression lowers to a
	// box_replace on the per-entry key, but the per-entry box is only created
	// lazily. Emit a box_create(key, total_size) as a pending pre-statement
	// so the box exists before box_replace runs. Idempotent when the box
	// already exists with the same size.
	// Also handles nested field writes: `n[k][i].field = v` where target is a
	// FieldExpression whose base chain resolves to IndexExpression-on-BoxValue.
	awst::IndexExpression const* boxIdx = nullptr;
	{
		awst::Expression const* cur = target.get();
		while (cur)
		{
			if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(cur))
			{
				if (dynamic_cast<awst::BoxValueExpression const*>(idx->base.get()))
				{
					boxIdx = idx;
					break;
				}
				cur = idx->base.get();
			}
			else if (auto const* fe = dynamic_cast<awst::FieldExpression const*>(cur))
			{
				cur = fe->base.get();
			}
			else
			{
				break;
			}
		}
	}
	if (boxIdx)
	{
		auto const* idx = boxIdx;
		if (auto const* bv = dynamic_cast<awst::BoxValueExpression const*>(idx->base.get()))
		{
			if (bv->key && dynamic_cast<awst::BoxPrefixedKeyExpression const*>(bv->key.get()))
			{
				// Static array of dynamic-content elements: a zero-filled
				// box_create yields invalid ARC4 (head offsets all zero).
				// Pre-populate with the proper default encoding so subsequent
				// element splices have a valid head/tail layout to work with.
				bool dynamicArc4 = false;
				if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(bv->wtype))
					dynamicArc4 = TypeCoercion::arc4IsDynamic(sa);
				if (dynamicArc4)
				{
					if (auto enc = TypeCoercion::arc4DefaultEncoding(bv->wtype))
					{
						if (enc->size() > 0 && enc->size() <= 32768)
						{
							auto putCall = awst::makeIntrinsicCall(
								"box_put", awst::WType::voidType(), m_loc);
							putCall->stackArgs.push_back(bv->key);
							putCall->stackArgs.push_back(awst::makeBytesConstant(
								std::move(*enc), m_loc));
							auto putStmt = awst::makeExpressionStatement(std::move(putCall), m_loc);
							m_ctx.prePendingStatements.push_back(std::move(putStmt));
						}
					}
				}
				else
				{
					uint64_t totalSize = 0;
					if (bv->wtype)
					{
						if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(bv->wtype))
						{
							uint64_t elemSize = 32;
							if (auto const* elemT = sa->elementType())
							{
								if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(elemT))
									elemSize = std::max<uint64_t>(1u, static_cast<uint64_t>(uintN->n() / 8));
								else if (auto const* bw = dynamic_cast<awst::BytesWType const*>(elemT))
									if (bw->length().has_value())
										elemSize = *bw->length();
							}
							if (sa->arraySize() > 0)
								totalSize = elemSize * static_cast<uint64_t>(sa->arraySize());
						}
						else if (auto const* bw = dynamic_cast<awst::BytesWType const*>(bv->wtype))
						{
							if (bw->length().has_value() && *bw->length() > 0)
								totalSize = static_cast<uint64_t>(*bw->length());
						}
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
	}

	auto e = std::make_shared<awst::AssignmentExpression>();
	e->sourceLocation = m_loc;
	e->wtype = target->wtype;
	e->target = std::move(target);
	e->value = std::move(value);
	return e;
}

} // namespace puyasol::builder::sol_ast
