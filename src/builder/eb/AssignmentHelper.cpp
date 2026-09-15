/// @file AssignmentHelper.cpp
/// Compound assignment via builder pattern + ARC4 struct chain rebuild.

#include "builder/eb/AssignmentHelper.h"
#include "builder/eb/BuilderOps.h"
#include "builder/storage/StorageMapper.h"
#include "builder/codec/Arc4ArrayWidening.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/TypeMapper.h"
#include "awst/NameGen.h"

#include <libsolidity/ast/TypeProvider.h>
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
	auto binOp = binaryOpFor(_assignOp);
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
	if (computeEncodedElementSize(_structType).fixedBytes().value_or(0) > StorageMapper::kAvmStackValueMax)
	{
		// The backend supports a projected storage lvalue and can fold its
		// update to box_replace. Rebuilding this struct would first load its
		// potentially oversized siblings onto the stack.
		auto target = awst::makeWritableTarget(std::make_shared<awst::FieldExpression>(*_fieldExpr));
		ensureRootBoxPre(_ctx, target, _loc);
		return {std::move(target), std::move(_fieldValue), {}, true};
	}

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
			_ctx.typeMapper, std::make_shared<awst::FieldExpression>(*_fieldExpr), /*isResize=*/false, _loc))
		_ctx.queuePreEffect(std::move(stmt));

	return StructFieldCowStore{
		std::move(target), std::move(cow.assignValue), std::move(cow.fieldChain)};
}

std::shared_ptr<awst::Expression> AssignmentHelper::arc4EncodeForType(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _target,
	awst::SourceLocation const& _loc)
{
	if (_value->wtype == _target) return _value;

	bool const targetIsArc4 = builder::isArc4EncodedType(_target);
	if (!targetIsArc4) return _value;

	// Skip encode if types match structurally (TypeMapper may not intern pointers;
	// double-encoding would corrupt an ARC4 aggregate).
	bool sameShape = awst::structurallyEquivalent(
		_value->wtype, _target);
	if (sameShape) return _value;

	// Full recursive values and their finite projections have the same solc
	// identity, but different encodings. Repack their fields/elements instead
	// of retagging bytes or leaking a projection into the full-type cache.
	auto const* sourceSol = _ctx.typeMapper.solcAggregateFor(_value->wtype);
	auto const* targetSol = _ctx.typeMapper.solcAggregateFor(_target);
	using solidity::frontend::TypeProvider;
	using solidity::frontend::DataLocation;
	if (sourceSol && targetSol
		&& *TypeProvider::withLocationIfReference(DataLocation::Memory, sourceSol)
			== *TypeProvider::withLocationIfReference(DataLocation::Memory, targetSol))
	{
		if (auto const* targetStruct = dynamic_cast<awst::ARC4Struct const*>(_target))
		{
			auto source = awst::makeEvalOnce(std::move(_value), _loc);
			auto result = awst::makeNewStruct(_target, _loc);
			for (auto const& [name, type]: targetStruct->fields())
			{
				auto const* fieldType = awst::structFieldType(source->wtype, name);
				assert(fieldType);
				std::shared_ptr<awst::Expression> field = awst::makeFieldExpression(source, name, fieldType, _loc);
				// An opaque recursive field stores the complete encoded subtree.
				if (type == awst::WType::bytesType() && isArc4EncodedType(fieldType))
					field = awst::makeAsBytes(std::move(field), _loc);
				else if (fieldType == awst::WType::bytesType() && isArc4EncodedType(type))
					field = awst::makeReinterpretCast(std::move(field), type, _loc);
				else
					field = arc4EncodeForType(_ctx, std::move(field), type, _loc);
				result->values.emplace(name, std::move(field));
			}
			return result;
		}
		auto const* targetElement = awst::arrayElementType(_target);
		auto const* sourceElement = awst::arrayElementType(_value->wtype);
		if (targetElement && sourceElement)
		{
			if (auto const* literal = dynamic_cast<awst::NewArray const*>(_value.get()))
			{
				auto result = awst::makeNewArray(_target, _loc);
				for (auto const& element: literal->values)
					result->values.push_back(arc4EncodeForType(_ctx, element, targetElement, _loc));
				return result;
			}
			std::string const id = std::to_string(awst::NameGen::next("RecursiveProjection"));
			auto const* arrayType = _ctx.typeMapper.createType<awst::ARC4DynamicArray>(targetElement);
			auto result = awst::makeVarExpression("__projection_out_" + id, arrayType, _loc);
			_ctx.queuePreEffect(awst::makeAssignmentStatement(
				result, awst::makeNewArray(arrayType, _loc), _loc));
			auto item = awst::makeVarExpression("__projection_item_" + id, sourceElement, _loc);
			auto lowered = _ctx.lowerOperand([&] {
				return arc4EncodeForType(_ctx, item, targetElement, _loc);
			});
			auto loop = awst::makeNode<awst::ForInLoop>(_loc);
			loop->sequence = std::move(_value);
			loop->items = std::move(item);
			loop->loopBody = awst::makeBlock(_loc);
			loop->loopBody->body = std::move(lowered.effects.pre);
			loop->loopBody->body.push_back(awst::makeExpressionStatement(
				awst::makeArrayPushOne(result, std::move(lowered.value), arrayType, _loc), _loc));
			assert(lowered.effects.post.empty());
			_ctx.queuePreEffect(std::move(loop));
			return awst::makeConvertArray(std::move(result), _target, _loc);
		}
	}

	_value = builder::TypeCoercion::stringToBytes(std::move(_value), _loc);

	// Representation-only callers share the conversion emitter used by the
	// typed ConversionPlan. No speculative source bindings on a failed match.
	bool const sourceIsArray = _value->wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| _value->wtype->kind() == awst::WTypeKind::ARC4DynamicArray;
	bool const targetIsArray = _target->kind() == awst::WTypeKind::ARC4StaticArray
		|| _target->kind() == awst::WTypeKind::ARC4DynamicArray;
	if (sourceIsArray && targetIsArray)
		return builder::TypeCoercion::coerceForAssignment(
			std::move(_value), _target, _loc, &_ctx.preEffects());

	// Native signed values use canonical 256-bit two's complement, while a
	// signed ARC4 field carries only its declared N bits. Puya's uintN encoder
	// checks overflow, so strip the sign fill before encoding that wire value.
	if (auto const* integer = dynamic_cast<awst::ARC4UIntN const*>(_target);
		integer && integer->isSigned() && integer->n() < 256
		&& _value->wtype == awst::WType::biguintType())
		_value = builder::TypeCoercion::maskUnsignedToWidth(std::move(_value), integer->n(), _loc);

	// Narrowing: uint64 → arc4.uintN (N < 64).
	if (auto narrowed = builder::tryNarrowUInt64ToArc4UIntN(
			_value, _target, _loc))
		return narrowed;

	// bytes/string → dynamic ARC4 byte-array (arc4.string / arc4.dynamic_bytes / uint8[]):
	// puya rejects makeARC4Encode(bytes, arc4.string) ("cannot encode bytes to (len+utf8[])").
	// Build [uint16 len][raw bytes] directly and reinterpret.
	// e.g. `string[] s; s[0] = "hi"` hits this path.
	// (Inverse of the abi.encode string-element fix in encodeFromArc4Bytes.)
	if (_target->kind() == awst::WTypeKind::ARC4DynamicArray
		&& (_value->wtype == awst::WType::bytesType()
			|| (_value->wtype && _value->wtype->kind() == awst::WTypeKind::Bytes)))
	{
		auto const* da = static_cast<awst::ARC4DynamicArray const*>(_target);
		if (da->elementType()
			&& ::puyasol::builder::computeEncodedElementSize(da->elementType()).fixedBytes() == 1)
		{
			auto once = awst::makeEvalOnce(std::move(_value), _loc);
			auto header = awst::makeUInt16Bytes(awst::makeLen(once, _loc), _loc);
			auto arc4Bytes = awst::makeConcat(std::move(header), once, _loc);
			return awst::makeReinterpretCast(std::move(arc4Bytes), _target, _loc);
		}
	}

	// Fixed bytes / function pointers already have the static array's byte encoding.
	if (_target->kind() == awst::WTypeKind::ARC4StaticArray
		&& _value->wtype->kind() == awst::WTypeKind::Bytes)
		return awst::makeARC4FromBytes(std::move(_value), _target, _loc);
	return awst::makeARC4Encode(std::move(_value), _target, _loc);
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
	_value = arc4EncodeForType(_ctx, std::move(_value), _target->wtype, _loc);
	ensureRootBoxPre(_ctx, _target, _loc);
	return PlainStore{std::move(_target), std::move(_value)};
}

} // namespace puyasol::builder::eb
