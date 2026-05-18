/// @file SolAssignment.cpp
/// Top-level assignment translator. Dispatches through a named pipeline
/// of phases (try*/apply*) — each phase's body lives further down in
/// this file. Shape-specific handlers (tuple, struct-field, bytes-elem,
/// storage-pointer reassign, etc.) live in sibling SolAssignment*.cpp.

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
namespace
{
/// Detect mapping-derived box keys: either the legacy `BoxPrefixedKey(prefix, sha256(...))`
/// shape, or the per-layer `sha256(keyBytes ++ parent)` shape. Used to gate the
/// pre-create-the-per-entry-box logic for `mapping(K => sized_type) m; m[k] ... = v`.
bool isMappingDerivedKey(awst::Expression const* _key)
{
	if (!_key) return false;
	if (dynamic_cast<awst::BoxPrefixedKeyExpression const*>(_key)) return true;
	if (auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(_key))
		return ic->opCode == "sha256";
	return false;
}
} // namespace


using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolAssignment::SolAssignment(eb::ContractContext& _ctx, Assignment const& _node)
	: SolExpression(_ctx, _node), m_assignment(_node)
{
}

// ─────────────────────────────────────────────────────────────────────
// toAwst — orchestrator. The implicit pipeline:
//   (1) LHS-shape early-outs that don't even need buildExpr (transient
//       state, storage-pointer reassign, multi-box write, push-assign)
//   (2) Build target + value
//   (3) Per-shape early-outs that consume target/value (enum check, slot
//       writes, tuple decomp, bytes-elem, struct field, named tuple)
//   (4) Generic finalization: compound op, type coercion, lvalue
//       normalization, ARC4 encode, box pre-populate, emit assignment
// ─────────────────────────────────────────────────────────────────────
std::shared_ptr<awst::Expression> SolAssignment::toAwst()
{
	Token op = m_assignment.assignmentOperator();

	// (1) Pre-buildExpr early-outs.
	if (auto r = tryHandleTransientStateWrite())     return std::move(*r);
	if (auto r = tryHandleStoragePointerReassign())  return std::move(*r);
	if (auto r = tryHandleMultiBoxArrayWrite())      return std::move(*r);
	if (auto r = tryHandlePushAssignRewrite(op))     return std::move(*r);

	// (2) Build target + value (`tryHandlePushAssignRewrite` would have
	// done LHS build itself and returned the ArrayExtend if it claimed).
	auto target = buildExpr(m_assignment.leftHandSide());
	auto value = buildExpr(m_assignment.rightHandSide());

	// (3) Per-shape early-outs.
	value = applyEnumRangeCheck(std::move(value), op);
	if (auto r = trySlotBasedArrayWrite(op, target, value))                     return std::move(*r);
	if (auto r = trySlotBasedScalarWrite(op, target, value))                    return std::move(*r);
	if (auto r = tryTupleAssignment(target, value))                             return std::move(*r);
	if (auto r = tryBytesElemAssignment(target, value))                         return std::move(*r);
	if (auto r = tryStructOrNamedTupleFieldAssignment(op, target, value))       return std::move(*r);

	// (4) Generic finalization.
	value = applyCompoundAssignment(op, target, std::move(value));
	value = applyAssignmentTypeCoercion(std::move(value), target);
	target = awst::makeWritableTarget(std::move(target));
	value = applyArc4EncodeIfNeeded(std::move(value), target);
	maybePrePopulateBox(target);
	return awst::makeAssignmentExpression(std::move(target), std::move(value), m_loc);
}

// ─────────────────────────────────────────────────────────────────────
// Phase implementations
// ─────────────────────────────────────────────────────────────────────

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandlePushAssignRewrite(Token _op)
{
	// `arr.push() = value` shape: Solidity's arg-less push returns a
	// reference to the new slot. Since we don't model references, stash
	// the RHS as `pendingArrayPushValue` BEFORE the LHS build, so
	// SolArrayMethod::push folds it into the ArrayExtend instead of
	// emitting a default-valued push + VoidConstant.
	if (_op != Token::Assign) return std::nullopt;
	auto const* lhsCall = dynamic_cast<FunctionCall const*>(&m_assignment.leftHandSide());
	if (!lhsCall || !lhsCall->arguments().empty()) return std::nullopt;
	auto const* member = dynamic_cast<MemberAccess const*>(&lhsCall->expression());
	if (!member || member->memberName() != "push") return std::nullopt;

	m_ctx.pendingArrayPushValue = buildExpr(m_assignment.rightHandSide());
	auto target = buildExpr(m_assignment.leftHandSide());
	m_ctx.pendingArrayPushValue.reset();
	// `target` is now the ArrayExtend expression emitted by SolArrayMethod.
	return target;
}

std::shared_ptr<awst::Expression>
SolAssignment::applyEnumRangeCheck(std::shared_ptr<awst::Expression> _value, Token _op)
{
	// EVM panics (0x21) on assigning out-of-range enum values. Pre-emit
	// an assert if LHS is an enum type.
	if (_op != Token::Assign) return _value;
	auto const* lhsType = m_assignment.leftHandSide().annotation().type;
	auto const* enumType = dynamic_cast<EnumType const*>(lhsType);
	if (!enumType) return _value;

	unsigned numMembers = enumType->numberOfMembers();
	auto val = builder::TypeCoercion::implicitNumericCast(_value, awst::WType::uint64Type(), m_loc);
	auto maxVal = awst::makeIntegerConstant(numMembers, m_loc);
	auto cmp = awst::makeNumericCompare(val, awst::NumericComparison::Lt, std::move(maxVal), m_loc);
	m_ctx.queuePreStmt(awst::makeAssert(std::move(cmp), m_loc, "enum out of range"), m_loc);
	return val;
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::trySlotBasedArrayWrite(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression> const& _value)
{
	// Slot-based storage write: target is biguint (slot offset), value
	// is a static-sized array. Expand to per-element
	// `__storage_write(slot + j, value[j])` calls.
	if (_op != Token::Assign || _target->wtype != awst::WType::biguintType()) return std::nullopt;
	auto const* lhsType = m_assignment.leftHandSide().annotation().type;
	auto const* arrType = lhsType ? dynamic_cast<ArrayType const*>(lhsType) : nullptr;
	if (!arrType)
	{
		auto const* rhsType = m_assignment.rightHandSide().annotation().type;
		arrType = rhsType ? dynamic_cast<ArrayType const*>(rhsType) : nullptr;
	}
	if (!arrType || arrType->isDynamicallySized()) return std::nullopt;

	unsigned len = static_cast<unsigned>(arrType->length());
	for (unsigned j = 0; j < len; ++j)
	{
		auto jConst = awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType());
		auto slotJ = awst::makeBigUIntBinOp(_target, awst::BigUIntBinaryOperator::Add, std::move(jConst), m_loc);
		auto castBytes = awst::makeReinterpretCast(std::move(slotJ), awst::WType::bytesType(), m_loc);
		auto last8 = awst::makeExtractLastN(std::move(castBytes), 8, m_loc);
		auto btoi = awst::makeBtoi(std::move(last8), m_loc);

		auto idx = awst::makeIntegerConstant(j, m_loc);
		awst::WType const* elemWtype;
		if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_value->wtype))
			elemWtype = sa->elementType();
		else if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(_value->wtype))
			elemWtype = da->elementType();
		else
			elemWtype = m_ctx.typeMapper.map(arrType->baseType());

		auto elemExpr = awst::makeIndexExpression(_value, std::move(idx), elemWtype, m_loc);

		std::shared_ptr<awst::Expression> elemVal = std::move(elemExpr);
		if (elemVal->wtype && elemVal->wtype->kind() == awst::WTypeKind::ARC4UIntN)
			elemVal = awst::makeARC4Decode(std::move(elemVal), awst::WType::biguintType(), m_loc);
		else if (elemVal->wtype == awst::WType::uint64Type())
		{
			auto itob = awst::makeItob(std::move(elemVal), m_loc);
			elemVal = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
		}

		auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), m_loc);
		awst::pushCallArg(call->args, "__slot", std::move(btoi));
		awst::pushCallArg(call->args, "__value", std::move(elemVal));
		m_ctx.queueStmt(std::move(call), m_loc);
	}
	return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc, awst::WType::biguintType())};
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::trySlotBasedScalarWrite(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	// Scalar slot-based storage write: target is a computed biguint slot.
	// Emit `__storage_write(btoi(slot), value)` directly.
	if (!dynamic_cast<awst::BigUIntBinaryOperation const*>(_target.get())
		|| _target->wtype != awst::WType::biguintType())
		return std::nullopt;

	// Compound: read current first, apply op.
	if (_op != Token::Assign)
	{
		auto readSlot = builder::StorageMapper::biguintSlotToBtoi(_target, m_loc);
		auto readCall = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), m_loc);
		awst::pushCallArg(readCall->args, "__slot", std::move(readSlot));

		auto* targetSolType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, _op, targetSolType, readCall, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(_op, std::move(readCall), std::move(_value),
				_target->wtype, m_loc);
	}

	auto btoi = builder::StorageMapper::biguintSlotToBtoi(_target, m_loc);
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), m_loc);
	awst::pushCallArg(call->args, "__slot", std::move(btoi));
	awst::pushCallArg(call->args, "__value", std::move(_value));
	m_ctx.queueStmt(std::move(call), m_loc);
	return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc, awst::WType::biguintType())};
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryTupleAssignment(
	std::shared_ptr<awst::Expression>& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	if (!dynamic_cast<awst::TupleExpression const*>(_target.get())) return std::nullopt;
	auto const* sourceLhs = dynamic_cast<solidity::frontend::TupleExpression const*>(
		&m_assignment.leftHandSide());
	return handleTupleAssignment(std::move(_target), std::move(_value), sourceLhs);
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryBytesElemAssignment(
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	auto const* indexExpr = dynamic_cast<awst::IndexExpression const*>(_target.get());
	if (!indexExpr || !indexExpr->base || !indexExpr->base->wtype
		|| indexExpr->base->wtype->kind() != awst::WTypeKind::Bytes)
		return std::nullopt;
	return handleBytesElementAssignment(indexExpr, std::move(_value));
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryStructOrNamedTupleFieldAssignment(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	// Unwrap ARC4Decode for Lvalue purposes
	auto unwrappedTarget = _target;
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(_target.get()))
		unwrappedTarget = decodeExpr->value;

	auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(unwrappedTarget.get());
	if (!fieldExpr) return std::nullopt;

	// ARC4Struct field: copy-on-write write-back.
	auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
	if (!arc4StructType)
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
			arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
	if (arc4StructType)
		return handleStructFieldAssignment(fieldExpr, std::move(_value), unwrappedTarget);

	// Named-WTuple field assignment: `t.field = v` where t is a WTuple
	// with named members (return-tuple style).
	auto const* tupleType = dynamic_cast<awst::WTuple const*>(fieldExpr->base->wtype);
	if (!tupleType || !tupleType->names().has_value()) return std::nullopt;

	auto base = fieldExpr->base;
	std::string fieldName = fieldExpr->name;

	if (_op != Token::Assign)
	{
		auto currentField = awst::makeFieldExpression(base, fieldName, fieldExpr->wtype, m_loc);
		auto* solType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, _op, solType, currentField, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(_op, std::move(currentField), std::move(_value),
				fieldExpr->wtype, m_loc);
	}

	_value = builder::TypeCoercion::implicitNumericCast(
		std::move(_value), fieldExpr->wtype, m_loc);
	auto newTuple = buildTupleWithUpdatedField(base, fieldName, std::move(_value));

	auto writeTarget = base;
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
		writeTarget = sg->field;

	auto e = awst::makeAssignmentExpression(std::move(writeTarget), std::move(newTuple), m_loc);
	return awst::makeFieldExpression(std::move(e), fieldName, fieldExpr->wtype, m_loc);
}

std::shared_ptr<awst::Expression>
SolAssignment::applyCompoundAssignment(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression> _value)
{
	if (_op == Token::Assign) return _value;

	auto currentValue = buildExpr(m_assignment.leftHandSide());
	if (dynamic_cast<awst::BoxValueExpression const*>(currentValue.get()))
		currentValue = builder::StorageMapper::makeStateGetWithDefault(currentValue, currentValue->wtype, m_loc);
	auto* targetSolType = m_assignment.leftHandSide().annotation().type;
	auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
		m_ctx, _op, targetSolType, currentValue, _value, m_loc);
	if (builderResult)
		return builderResult;
	return m_ctx.buildBinaryOp(_op, std::move(currentValue), std::move(_value),
		_target->wtype, m_loc);
}

std::shared_ptr<awst::Expression>
SolAssignment::applyAssignmentTypeCoercion(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> const& _target)
{
	// Handles int→bytes[N], string→bytes, numeric casts.
	_value = builder::TypeCoercion::coerceForAssignment(std::move(_value), _target->wtype, m_loc);
	if (_value->wtype != _target->wtype && _target->wtype
		&& _target->wtype->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_target->wtype);
		auto const* strConst = dynamic_cast<awst::StringConstant const*>(_value.get());
		if (bytesType && bytesType->length().has_value() && *bytesType->length() > 0 && strConst)
		{
			if (auto padded = builder::TypeCoercion::stringToBytesN(
					_value.get(), _target->wtype, *bytesType->length(), m_loc))
				_value = std::move(padded);
		}
		else
		{
			bool valueIsCompatible = _value->wtype == awst::WType::stringType()
				|| (_value->wtype && _value->wtype->kind() == awst::WTypeKind::Bytes);
			if (valueIsCompatible)
				_value = awst::makeReinterpretCast(std::move(_value), _target->wtype, m_loc);
		}
	}
	if (_value->wtype != _target->wtype && _target->wtype == awst::WType::stringType()
		&& _value->wtype && (_value->wtype->kind() == awst::WTypeKind::Bytes
			|| _value->wtype == awst::WType::bytesType()))
	{
		_value = awst::makeReinterpretCast(std::move(_value), _target->wtype, m_loc);
	}
	return _value;
}

std::shared_ptr<awst::Expression>
SolAssignment::applyArc4EncodeIfNeeded(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> const& _target)
{
	if (_value->wtype == _target->wtype) return _value;

	bool targetIsArc4 = false;
	switch (_target->wtype->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
	case awst::WTypeKind::ARC4StaticArray:
	case awst::WTypeKind::ARC4DynamicArray:
	case awst::WTypeKind::ARC4Struct:
		targetIsArc4 = true; break;
	default: break;
	}
	if (!targetIsArc4) return _value;

	// If value is already ARC4 with structurally matching type (pointers
	// differ only because TypeMapper didn't intern), skip the redundant
	// encode — it would otherwise double-encode an ARC4 aggregate.
	bool sameShape = _value->wtype->kind() == _target->wtype->kind()
		&& _value->wtype->name() == _target->wtype->name();
	if (sameShape) return _value;

	_value = builder::TypeCoercion::stringToBytes(std::move(_value), m_loc);

	// Array element-wise widening: arc4.{static,dynamic}_array<arc4.intM, ...>
	// → arc4.{static,dynamic}_array<arc4.intN, ...> with M < N has no
	// puya codec. Pin source bytes to a temp (TypeCoercion would
	// otherwise read it multiple times) and call into the helper.
	bool const sourceIsArc4Array =
		_value->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _value->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
	bool const targetIsArc4Array =
		_target->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _target->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
	if (sourceIsArc4Array && targetIsArc4Array)
	{
		static int s_widCounter = 0;
		std::string tmpName = "__widen_src_" + std::to_string(s_widCounter++);
		auto srcAsBytes = awst::makeReinterpretCast(_value, awst::WType::bytesType(), m_loc);
		auto tmpVar = awst::makeVarExpression(tmpName, awst::WType::bytesType(), m_loc);
		m_ctx.prePendingStatements.push_back(
			awst::makeAssignmentStatement(tmpVar, std::move(srcAsBytes), m_loc));
		auto const* sourceType = _value->wtype;
		auto mkSrc = [&]() {
			return awst::makeVarExpression(tmpName, awst::WType::bytesType(), m_loc);
		};
		std::shared_ptr<awst::Expression> widened;
		if (_target->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
			widened = builder::TypeCoercion::tryWidenArc4StaticArrayInt(
				sourceType, _target->wtype, mkSrc, m_loc);
		else
			widened = builder::TypeCoercion::tryWidenArc4DynamicArrayInt(
				sourceType, _target->wtype, mkSrc,
				[this](std::shared_ptr<awst::Statement> _s) {
					m_ctx.prePendingStatements.push_back(std::move(_s));
				},
				m_loc);
		if (widened) return widened;
	}

	// Narrowing: uint64 → arc4.uintN where N < 64.
	if (auto narrowed = builder::TypeCoercion::tryNarrowUInt64ToArc4UIntN(
			_value, _target->wtype, m_loc))
		return narrowed;

	return awst::makeARC4Encode(std::move(_value), _target->wtype, m_loc);
}

void SolAssignment::maybePrePopulateBox(
	std::shared_ptr<awst::Expression> const& _target)
{
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
		awst::Expression const* cur = _target.get();
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
				cur = fe->base.get();
			else
				break;
		}
	}
	if (!boxIdx) return;

	auto const* bv = dynamic_cast<awst::BoxValueExpression const*>(boxIdx->base.get());
	if (!bv || !bv->key
		|| !isMappingDerivedKey(bv->key.get()))
		return;

	// Static array of dynamic-content elements: a zero-filled box_create
	// yields invalid ARC4 (head offsets all zero). Pre-populate with the
	// proper default encoding so subsequent element splices have a valid
	// head/tail layout to work with. Gate on `!box_exists(key)` so a
	// subsequent assignment after the box has grown (e.g. via .push() loop)
	// doesn't try to box_put the smaller default and trip
	// "wrong size N != M".
	bool dynamicArc4 = false;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(bv->wtype))
		dynamicArc4 = TypeCoercion::arc4IsDynamic(sa);
	if (dynamicArc4)
	{
		auto enc = TypeCoercion::arc4DefaultEncoding(bv->wtype);
		if (!enc || enc->size() == 0 || enc->size() > 32768) return;

		auto putCall = awst::makeIntrinsicCall("box_put", awst::WType::voidType(), m_loc);
		putCall->stackArgs.push_back(bv->key);
		putCall->stackArgs.push_back(awst::makeBytesConstant(std::move(*enc), m_loc));

		auto* tupleType = m_ctx.typeMapper.template createType<awst::WTuple>(
			std::vector<awst::WType const*>{
				awst::WType::uint64Type(), awst::WType::boolType()});
		auto boxLen = awst::makeIntrinsicCall("box_len", tupleType, m_loc);
		boxLen->stackArgs.push_back(bv->key);
		auto exists = awst::makeTupleItem(
			std::move(boxLen), 1, awst::WType::boolType(), m_loc);
		auto notExists = awst::makeNot(std::move(exists), m_loc);

		auto thenBlock = awst::makeBlock(m_loc);
		thenBlock->body.push_back(awst::makeExpressionStatement(std::move(putCall), m_loc));
		auto elseBlock = awst::makeBlock(m_loc);
		auto ifStmt = awst::makeIfElse(
			std::move(notExists), std::move(thenBlock), std::move(elseBlock), m_loc);
		m_ctx.queuePrePending(std::move(ifStmt));
		return;
	}

	// Static-sized scalar/bytes box: idempotent box_create at total_size.
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
		auto createCall = awst::makeIntrinsicCall("box_create", awst::WType::boolType(), m_loc);
		createCall->stackArgs.push_back(bv->key);
		createCall->stackArgs.push_back(awst::makeIntegerConstant(totalSize, m_loc));
		m_ctx.queuePreStmt(std::move(createCall), m_loc);
	}
}

} // namespace puyasol::builder::sol_ast
