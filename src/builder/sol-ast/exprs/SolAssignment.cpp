/// @file SolAssignment.cpp
/// Top-level assignment translator (try*/apply* pipeline).
/// Shape-specific handlers live in sibling SolAssignment*.cpp.

#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4ArrayWidening.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-ast/exprs/SolIndexAccess.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolAssignment::SolAssignment(eb::ContractContext& _ctx, Assignment const& _node)
	: SolExpression(_ctx, _node), m_assignment(_node)
{
}

// toAwst pipeline:
//   (1) Pre-buildExpr early-outs (transient, storage-ptr, multi-box, push-assign)
//   (2) Build target + value
//   (3) Per-shape early-outs (enum check, slot writes, tuple, bytes-elem, struct/WTuple)
//   (4) Generic finalization (compound op, coerce, lvalue norm, ARC4 encode, box pre-populate)
std::shared_ptr<awst::Expression> SolAssignment::toAwst()
{
	Token op = m_assignment.assignmentOperator();

	// (1) Pre-buildExpr early-outs.
	if (auto r = tryHandleTransientStateWrite())     return std::move(*r);
	if (auto r = tryHandleStoragePointerReassign())  return std::move(*r);
	if (auto r = tryHandleMultiBoxArrayWrite())      return std::move(*r);
	if (auto r = tryHandleBoxedArrayElemWrite())     return std::move(*r);
	if (auto r = tryHandleOffsetStructRefFieldWrite()) return std::move(*r);
	if (auto r = tryHandleBlobAggregateWrite())      return std::move(*r);
	if (auto r = tryHandlePushAssignRewrite(op))     return std::move(*r);

	// (2) Build target + value (if tryHandlePushAssignRewrite claimed, it already returned).
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

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandlePushAssignRewrite(Token _op)
{
	// `arr.push() = value`: stash RHS as pendingArrayPushValue before LHS build so
	// SolArrayMethod::push folds it into ArrayExtend (we don't model Solidity refs).
	if (_op != Token::Assign) return std::nullopt;
	auto const* lhsCall = dynamic_cast<FunctionCall const*>(&m_assignment.leftHandSide());
	if (!lhsCall || !lhsCall->arguments().empty()) return std::nullopt;
	auto const* member = dynamic_cast<MemberAccess const*>(&lhsCall->expression());
	if (!member || member->memberName() != "push") return std::nullopt;

	m_ctx.pendingArrayPushValue = buildExpr(m_assignment.rightHandSide());
	auto target = buildExpr(m_assignment.leftHandSide());
	m_ctx.pendingArrayPushValue.reset();
	return target; // ArrayExtend emitted by SolArrayMethod
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleBlobAggregateWrite()
{
	// Writes into a blob-backed aggregate: scalar leaves (`a[i]=v`, `p.f.x=v`)
	// and struct/array-valued copies (`p.w1 = bytesToG1Point(...)`).
	if (m_assignment.assignmentOperator() != Token::Assign)
		return std::nullopt;
	auto const& lhs = m_assignment.leftHandSide();
	auto const* lhsType = lhs.annotation().type;
	if (!lhsType)
		return std::nullopt;
	auto off = SolIndexAccess::resolveBlobOffset(m_ctx, m_scope, lhs, m_loc);
	if (!off)
		return std::nullopt;

	// Struct/array copy: write word-by-word via writeMemWordDirect.
	// Only 32-byte-aligned aggregates (e.g. Honk G1Point = 2×uint256 = 64 B).
	if (dynamic_cast<ArrayType const*>(lhsType) || dynamic_cast<StructType const*>(lhsType))
	{
		int sz = builder::computeEncodedElementSize(m_ctx.typeMapper.map(lhsType));
		if (sz <= 0 || sz % 32 != 0)
			return std::nullopt;
		auto agg = buildExpr(m_assignment.rightHandSide());
		auto aggBytes = awst::makeAsBytes(std::move(agg), m_loc);
		std::string offN = "__blobwa_off_" + std::to_string(m_assignment.id());
		std::string vN = "__blobwa_v_" + std::to_string(m_assignment.id());
		m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(offN, awst::WType::uint64Type(), m_loc), std::move(off), m_loc));
		m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(vN, awst::WType::bytesType(), m_loc), std::move(aggBytes), m_loc));
		for (int i = 0; i * 32 < sz; ++i)
		{
			auto wordOff = awst::makeUInt64BinOp(
				awst::makeVarExpression(offN, awst::WType::uint64Type(), m_loc),
				awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(static_cast<uint64_t>(i * 32), m_loc), m_loc);
			auto word = awst::makeExtract3(
				awst::makeVarExpression(vN, awst::WType::bytesType(), m_loc),
				awst::makeIntegerConstant(static_cast<uint64_t>(i * 32), m_loc),
				awst::makeIntegerConstant("32", m_loc), m_loc);
			builder::AssemblyBuilder::writeMemWordDirect(
				std::move(wordOff), std::move(word), m_loc, m_ctx.prePendingStatements);
		}
		return std::optional<std::shared_ptr<awst::Expression>>(
			awst::makeVarExpression(vN, awst::WType::bytesType(), m_loc));
	}

	// Materialise rhs; coerce to biguint so asBytes gives a valid 32-byte pad
	// (uint256/Fr stored as a full 32-byte EVM-memory word).
	auto v = buildExpr(m_assignment.rightHandSide());
	v = builder::TypeCoercion::implicitNumericCast(
		std::move(v), awst::WType::biguintType(), m_loc);
	std::string vN = "__blobassign_v_" + std::to_string(m_assignment.id());
	m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(vN, awst::WType::biguintType(), m_loc), std::move(v), m_loc));

	// Pad to exactly 32 big-endian bytes and write the blob word.
	auto vbytes = awst::makeExtractLastN(
		awst::makeLeftPad(awst::makeAsBytes(
			awst::makeVarExpression(vN, awst::WType::biguintType(), m_loc), m_loc), 32, m_loc), 32, m_loc);
	builder::AssemblyBuilder::writeMemWordDirect(
		std::move(off), std::move(vbytes), m_loc, m_ctx.prePendingStatements);

	return std::optional<std::shared_ptr<awst::Expression>>(
		awst::makeVarExpression(vN, awst::WType::biguintType(), m_loc));
}

std::shared_ptr<awst::Expression>
SolAssignment::applyEnumRangeCheck(std::shared_ptr<awst::Expression> _value, Token _op)
{
	// EVM panic 0x21 on out-of-range enum assign; pre-emit assert.
	if (_op != Token::Assign) return _value;
	auto const* lhsType = m_assignment.leftHandSide().annotation().type;
	auto const* enumType = dynamic_cast<EnumType const*>(lhsType);
	if (!enumType) return _value;

	unsigned numMembers = enumType->numberOfMembers();
	auto val = builder::TypeCoercion::implicitNumericCast(_value, awst::WType::uint64Type(), m_loc);
	m_ctx.queuePreStmt(awst::makeEnumRangeAssert(val, numMembers, m_loc), m_loc);
	return val;
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::trySlotBasedArrayWrite(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression> const& _value)
{
	// Slot-based array write: target is biguint slot offset, expand to
	// per-element __storage_write(slot+j, value[j]).
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
		auto castBytes = awst::makeAsBytes(std::move(slotJ), m_loc);
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
			elemVal = awst::makeAsBiguint(std::move(itob), m_loc);
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
	// Scalar slot-based write: emit __storage_write(btoi(slot), value).
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

	// ARC4Struct field: copy-on-write.
	auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
	if (!arc4StructType)
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
			arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
	if (arc4StructType)
		return handleStructFieldAssignment(fieldExpr, std::move(_value), unwrappedTarget);

	// Named-WTuple field assignment (`t.field = v`, return-tuple style).
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

	auto writeTarget = awst::unwrapStateGet(base);

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

	// Reuse the already-built target to avoid re-evaluating a side-effecting
	// index (e.g. `arr[i++] += 5` gave i==2 when LHS was rebuilt). The built
	// target is the read form; wrap BoxValue in StateGet to read the stored value.
	// makeWritableTarget in toAwst still yields the write target from the same node.
	auto currentValue = _target;
	auto* targetSolType = m_assignment.leftHandSide().annotation().type;
	if (dynamic_cast<awst::BoxValueExpression const*>(currentValue.get()))
		currentValue = builder::StorageMapper::makeStateGetWithDefault(currentValue, currentValue->wtype, m_loc);
	else if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(currentValue.get()))
	{
		// Storage dynamic-array element: the write-form indexes a box and yields the ARC4-ENCODED
		// element (Encoded(uintN)); the compound arithmetic itob's that and fails. Decode to the native
		// value (memory/calldata index exprs already carry native values, so gate on a box base; the
		// later applyArc4EncodeIfNeeded re-encodes the result). Mirrors the read path's decode.
		if (dynamic_cast<awst::BoxValueExpression const*>(idx->base.get()))
		{
			auto* nativeType = m_ctx.typeMapper.map(targetSolType);
			if (currentValue->wtype != nativeType)
				currentValue = builder::TypeCoercion::signExtendSignedElement(
					awst::makeARC4Decode(currentValue, nativeType, m_loc), targetSolType, m_loc);
		}
	}
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
	// int→bytes[N], string→bytes, numeric casts.
	_value = builder::TypeCoercion::coerceForAssignment(std::move(_value), _target->wtype, m_loc);
	// Signed sub-word → wider-signed implicit widen (`b = someInt8;` b:int16): coerceForAssignment
	// is a uint64→uint64 no-op that drops the sign. Re-extend from the RHS width. Plain `=` only
	// (compound `+=` coerces a same-typed computed value, not the raw RHS).
	if (m_assignment.assignmentOperator() == Token::Assign)
		_value = builder::TypeCoercion::signExtendSignedWiden(
			std::move(_value), m_assignment.rightHandSide().annotation().type,
			m_assignment.leftHandSide().annotation().type, m_loc);
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

	// Skip encode if types match structurally (TypeMapper may not intern pointers;
	// double-encoding would corrupt an ARC4 aggregate).
	bool sameShape = _value->wtype->kind() == _target->wtype->kind()
		&& _value->wtype->name() == _target->wtype->name();
	if (sameShape) return _value;

	_value = builder::TypeCoercion::stringToBytes(std::move(_value), m_loc);

	// Array element-wise widening: arc4 array<intM> → arc4 array<intN> (M<N) has no
	// puya codec. Pin source bytes to a temp and call the helper.
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
		auto srcAsBytes = awst::makeAsBytes(_value, m_loc);
		auto tmpVar = awst::makeVarExpression(tmpName, awst::WType::bytesType(), m_loc);
		m_ctx.prePendingStatements.push_back(
			awst::makeAssignmentStatement(tmpVar, std::move(srcAsBytes), m_loc));
		auto const* sourceType = _value->wtype;
		auto mkSrc = [&]() {
			return awst::makeVarExpression(tmpName, awst::WType::bytesType(), m_loc);
		};
		std::shared_ptr<awst::Expression> widened;
		if (_target->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
			widened = builder::tryWidenArc4StaticArrayInt(
				sourceType, _target->wtype, mkSrc, m_loc);
		else
			widened = builder::tryWidenArc4DynamicArrayInt(
				sourceType, _target->wtype, mkSrc,
				[this](std::shared_ptr<awst::Statement> _s) {
					m_ctx.prePendingStatements.push_back(std::move(_s));
				},
				m_loc);
		if (widened) return widened;
	}

	// Narrowing: uint64 → arc4.uintN (N < 64).
	if (auto narrowed = builder::tryNarrowUInt64ToArc4UIntN(
			_value, _target->wtype, m_loc))
		return narrowed;

	// bytes/string → dynamic ARC4 byte-array (arc4.string / arc4.dynamic_bytes / uint8[]):
	// puya rejects makeARC4Encode(bytes, arc4.string) ("cannot encode bytes to (len+utf8[])").
	// Build [uint16 len][raw bytes] directly and reinterpret.
	// e.g. `string[] s; s[0] = "hi"` hits this path.
	// (Inverse of the abi.encode string-element fix in encodeFromArc4Bytes.)
	if (_target->wtype->kind() == awst::WTypeKind::ARC4DynamicArray
		&& (_value->wtype == awst::WType::bytesType()
			|| (_value->wtype && _value->wtype->kind() == awst::WTypeKind::Bytes)))
	{
		auto const* da = static_cast<awst::ARC4DynamicArray const*>(_target->wtype);
		if (da->elementType()
			&& ::puyasol::builder::computeEncodedElementSize(da->elementType()) == 1)
		{
			auto once = awst::makeEvalOnce(std::move(_value), m_loc);
			auto header = awst::makeUInt16Bytes(awst::makeLen(once, m_loc), m_loc);
			auto arc4Bytes = awst::makeConcat(std::move(header), once, m_loc);
			return awst::makeReinterpretCast(std::move(arc4Bytes), _target->wtype, m_loc);
		}
	}

	return awst::makeARC4Encode(std::move(_value), _target->wtype, m_loc);
}

void SolAssignment::maybePrePopulateBox(
	std::shared_ptr<awst::Expression> const& _target)
{
	// Mapping-entry partial write: `m[k][i] = v` where m is `mapping(K => T[N])`
	// or `mapping(K => bytes[N])`. The per-entry box is lazy; emit box_create
	// as a pre-statement so box_replace finds it. Idempotent on same size.
	// Also handles `n[k][i].field = v` (FieldExpression chain rooted at IndexExpression-on-BoxValue).
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
		|| !builder::StorageMapper::isMappingDerivedKey(bv->key.get()))
		return;

	// Dynamic-element static array: zero-filled box_create gives invalid ARC4
	// (head offsets all zero). Pre-populate with proper default encoding.
	// Gated on !box_exists so a grown box (e.g. via .push()) isn't truncated
	// ("wrong size N != M").
	bool dynamicArc4 = false;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(bv->wtype))
		dynamicArc4 = arc4IsDynamic(sa);
	if (dynamicArc4)
	{
		auto enc = arc4DefaultEncoding(bv->wtype);
		if (!enc || enc->size() == 0 || enc->size() > 32768) return;

		auto putCall = awst::makeBoxPut(
			bv->key, awst::makeBytesConstant(std::move(*enc), m_loc), m_loc);

		auto boxLen = builder::StorageMapper::makeBoxLenTuple(
			m_ctx.typeMapper, bv->key, m_loc);
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

	// Static scalar/bytes box: idempotent box_create at total_size.
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
		auto createCall = awst::makeBoxCreate(
			bv->key, awst::makeIntegerConstant(totalSize, m_loc), m_loc);
		m_ctx.queuePreStmt(std::move(createCall), m_loc);
	}
}

} // namespace puyasol::builder::sol_ast
