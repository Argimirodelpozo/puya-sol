/// @file AssignmentHelper.cpp
/// Compound assignment via builder pattern + ARC4 struct chain rebuild.

#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-eb/BuilderRegistry.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/Arc4ArrayWidening.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/NameGen.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

std::shared_ptr<awst::Expression> AssignmentHelper::tryComputeCompoundValue(
	ContractContext& _ctx,
	solidity::frontend::Token _assignOp,
	solidity::frontend::Type const* _targetSolType,
	std::shared_ptr<awst::Expression> _currentValue,
	std::shared_ptr<awst::Expression> _rhs,
	awst::SourceLocation const& _loc)
{
	using Token = solidity::frontend::Token;

	auto mapOp = [](Token t) -> std::optional<BuilderBinaryOp> {
		switch (t)
		{
		case Token::AssignAdd: return BuilderBinaryOp::Add;
		case Token::AssignSub: return BuilderBinaryOp::Sub;
		case Token::AssignMul: return BuilderBinaryOp::Mult;
		// FloorDiv not Div: signed-division routing gates on FloorDiv (matching
		// SolBinaryOperation). Div would skip buildSignedModDiv for signed types
		// (-7/=2 gave 2^255-4 instead of -3). Unsigned: both lower identically.
		case Token::AssignDiv: return BuilderBinaryOp::FloorDiv;
		case Token::AssignMod: return BuilderBinaryOp::Mod;
		case Token::AssignShl: return BuilderBinaryOp::LShift;
		case Token::AssignShr: case Token::AssignSar: return BuilderBinaryOp::RShift;
		case Token::AssignBitOr: return BuilderBinaryOp::BitOr;
		case Token::AssignBitXor: return BuilderBinaryOp::BitXor;
		case Token::AssignBitAnd: return BuilderBinaryOp::BitAnd;
		default: return std::nullopt;
		}
	};

	auto binOp = mapOp(_assignOp);
	if (!binOp)
		return nullptr;

	if (!_targetSolType)
		return nullptr;

	auto leftBuilder = _ctx.builderForInstance(_targetSolType, _currentValue);
	if (!leftBuilder)
		return nullptr;

	auto rightBuilder = _ctx.builderForInstance(_targetSolType, _rhs);
	if (!rightBuilder)
		return nullptr;

	auto result = leftBuilder->binary_op(*rightBuilder, *binOp, _loc);
	if (!result)
		return nullptr;

	return result->resolve();
}

ArcStructCowResult AssignmentHelper::rebuildArc4StructChainCOW(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _initialTarget,
	std::shared_ptr<awst::Expression> _initialValue,
	awst::SourceLocation const& _loc)
{
	ArcStructCowResult result;
	result.assignTarget = std::move(_initialTarget);
	result.assignValue = std::move(_initialValue);

	while (auto const* outerField =
		dynamic_cast<awst::FieldExpression const*>(result.assignTarget.get()))
	{
		// Outer struct type — direct or through StateGet.
		auto const* outerStructType =
			dynamic_cast<awst::ARC4Struct const*>(outerField->base->wtype);
		if (!outerStructType)
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(outerField->base.get()))
				outerStructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
		if (!outerStructType) break;

		auto outerBase = outerField->base;
		// Write target: no StateGet wrapper (puya rejects it).
		auto outerWriteBase = awst::unwrapStateGet(outerBase);
		// Read base: BoxValue must be wrapped in StateGet for field reads.
		auto outerReadBase = outerBase;
		if (dynamic_cast<awst::BoxValueExpression const*>(outerWriteBase.get())
			&& !dynamic_cast<awst::StateGet const*>(outerBase.get()))
			outerReadBase = builder::StorageMapper::makeStateGetWithDefault(
				outerWriteBase, outerWriteBase->wtype, _loc);

		std::string outerFieldName = outerField->name;
		awst::WType const* outerFieldWtype = awst::structFieldType(outerStructType, outerFieldName);
		result.fieldChain.push_back({outerFieldName, outerFieldWtype});

		auto outerNewStruct = awst::makeNewStruct(outerStructType, _loc);
		for (auto const& [fn, ft]: outerStructType->fields())
		{
			if (fn == outerFieldName)
				outerNewStruct->values[fn] = std::move(result.assignValue);
			else
				outerNewStruct->values[fn] =
					awst::makeFieldExpression(outerReadBase, fn, ft, _loc);
		}
		result.assignTarget = std::move(outerWriteBase);
		result.assignValue = std::move(outerNewStruct);
	}

	return result;
}

std::shared_ptr<awst::Expression> AssignmentHelper::computeCompoundOrFallback(
	ContractContext& _ctx,
	solidity::frontend::Token _tryOp,
	solidity::frontend::Token _fallbackOp,
	solidity::frontend::Type const* _targetSolType,
	std::shared_ptr<awst::Expression> _current,
	std::shared_ptr<awst::Expression> _rhs,
	awst::WType const* _fallbackW,
	awst::SourceLocation const& _loc)
{
	if (auto computed = tryComputeCompoundValue(
			_ctx, _tryOp, _targetSolType, _current, _rhs, _loc))
		return computed;
	return _ctx.buildBinaryOp(
		_fallbackOp, std::move(_current), std::move(_rhs), _fallbackW, _loc);
}

AssignmentHelper::StructFieldCowStore AssignmentHelper::buildStructFieldCowStore(
	ContractContext& _ctx,
	awst::FieldExpression const* _fieldExpr,
	awst::ARC4Struct const* _structType,
	std::shared_ptr<awst::Expression> _fieldValue,
	awst::SourceLocation const& _loc)
{
	auto base = awst::unwrapStateGet(_fieldExpr->base);
	std::string fieldName = _fieldExpr->name;

	// Read the sibling fields with-default so a fresh (nonexistent) top-level
	// box yields defaults instead of reverting. rebuildArc4StructChainCOW only
	// wraps the read base for NESTED structs; a top-level bare BoxValue is
	// wrapped here. The write target stays the bare box (unwrapped).
	auto readBase = base;
	if (dynamic_cast<awst::BoxValueExpression const*>(base.get()))
		readBase = builder::StorageMapper::makeStateGetWithDefault(base, base->wtype, _loc);

	awst::WType const* arc4FieldType = awst::structFieldType(_structType, fieldName);
	if (arc4FieldType && _fieldValue->wtype != arc4FieldType)
		_fieldValue = awst::makeARC4Encode(std::move(_fieldValue), arc4FieldType, _loc);

	auto newStruct = awst::makeStructWithReplacedField(
		_structType, std::move(readBase), fieldName, std::move(_fieldValue), _loc);

	auto cow = rebuildArc4StructChainCOW(
		_ctx, std::move(base), std::move(newStruct), _loc);

	// Strip StateGet/ARC4Decode anywhere in the target chain (puya rejects
	// them as lvalues; covers the IndexExpression(StateGet(box), i) shape).
	auto target = awst::makeWritableTarget(std::move(cow.assignTarget));

	// Centralized box-lifecycle: the lazy mapping-entry box must exist before
	// box_replace. Shared with maybePrePopulateBox / SolArrayMethod::emitEnsureBox.
	if (auto stmt = builder::StorageMapper::makeEnsureRootBoxForWrite(
			_ctx.typeMapper, target, /*isResize=*/false, _loc))
		_ctx.queuePreEffect(std::move(stmt));

	return StructFieldCowStore{
		std::move(target), std::move(cow.assignValue), std::move(cow.fieldChain)};
}

std::shared_ptr<awst::Expression> AssignmentHelper::arc4EncodeForTarget(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> const& _target,
	awst::SourceLocation const& _loc)
{
	if (_value->wtype == _target->wtype) return _value;

	bool const targetIsArc4 = builder::isArc4EncodedType(_target->wtype);
	if (!targetIsArc4) return _value;

	// Skip encode if types match structurally (TypeMapper may not intern pointers;
	// double-encoding would corrupt an ARC4 aggregate).
	bool sameShape = awst::structurallyEquivalent(
		_value->wtype, _target->wtype);
	if (sameShape) return _value;

	_value = builder::TypeCoercion::stringToBytes(std::move(_value), _loc);

	// Representation-only callers share the conversion emitter used by the
	// typed ConversionPlan. No speculative source bindings on a failed match.
	bool const sourceIsArray = _value->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _value->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
	bool const targetIsArray = _target->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _target->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
	if (sourceIsArray && targetIsArray)
		return builder::TypeCoercion::coerceForAssignment(
			std::move(_value), _target->wtype, _loc, &_ctx.preEffects());

	// Narrowing: uint64 → arc4.uintN (N < 64).
	if (auto narrowed = builder::tryNarrowUInt64ToArc4UIntN(
			_value, _target->wtype, _loc))
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
			&& ::puyasol::builder::computeEncodedElementSize(da->elementType()).fixedBytes() == 1)
		{
			auto once = awst::makeEvalOnce(std::move(_value), _loc);
			auto header = awst::makeUInt16Bytes(awst::makeLen(once, _loc), _loc);
			auto arc4Bytes = awst::makeConcat(std::move(header), once, _loc);
			return awst::makeReinterpretCast(std::move(arc4Bytes), _target->wtype, _loc);
		}
	}

	return awst::makeARC4Encode(std::move(_value), _target->wtype, _loc);
}

void AssignmentHelper::ensureRootBoxPre(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> const& _target,
	awst::SourceLocation const& _loc)
{
	// Centralized: a PARTIAL element write into a lazily-created state-var or mapping-entry box needs
	// the root box to exist first (else box_replace hits "no such box"). See
	// StorageMapper::makeEnsureRootBoxForWrite (the single source of truth, shared with the push/pop
	// path in SolArrayMethod and the mapping-entry field write in SolAssignmentStructField).
	if (auto stmt = builder::StorageMapper::makeEnsureRootBoxForWrite(
			_ctx.typeMapper, _target, /*isResize=*/false, _loc))
		_ctx.queuePreEffect(std::move(stmt));
}

AssignmentHelper::PlainStore AssignmentHelper::preparePlainStore(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _target,
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc)
{
	_target = awst::makeWritableTarget(std::move(_target));
	_value = arc4EncodeForTarget(_ctx, std::move(_value), _target, _loc);
	ensureRootBoxPre(_ctx, _target, _loc);
	return PlainStore{std::move(_target), std::move(_value)};
}

} // namespace puyasol::builder::eb
