#pragma once

/// @file RefParamPassing.h
/// The ONE rule for how a declared reference param TRAVELS across a call
/// boundary. Method build (FunctionBuilder), freestanding build
/// (AWSTBuilder), and the call site (SolInternalCall) previously each
/// spelled it out — the comments at every copy warned that drift breaks
/// caller/callee arity and types (arity DID diverge once; see
/// isBoxKeyedStorageRef's "must match callee predicate" note).

#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-ast/StorageRefPointer.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

/// A fixed array too large to travel as one AVM value must pass its existing
/// box, not rely on backend inlining to recover a storage reference later.
inline bool isLargeFixedArrayRef(TypeMapper& _tm, solidity::frontend::Type const* _type)
{
	auto const* array = dynamic_cast<solidity::frontend::ArrayType const*>(_type);
	return array && !array->isDynamicallySized()
		&& computeEncodedElementSize(_tm.map(array)).fixedBytes().value_or(0) > 4096;
}

/// Classify from the declaration and cached source facts.
inline RefParamPassing classifyRefParamPassing(
	TypeMapper& _tm,
	solidity::frontend::VariableDeclaration const& _param,
	bool _isAsmSlotRef)
{
	using Loc = solidity::frontend::VariableDeclaration::Location;
	if (_tm.profile().evmStorageLayout
		&& _param.referenceLocation() == Loc::Storage)
		return RefParamPassing::SlotHandle;
	if (_param.referenceLocation() == Loc::Storage
		&& (isBoxKeyedStorageRef(_param.type(), _tm.analysis())
			|| isLargeFixedArrayRef(_tm, _param.type())
			|| _tm.analysis().structRefOffsetParams.contains(_param.id())
			|| _isAsmSlotRef)) // widened: plain structs + asm .slot refs
		return RefParamPassing::BoxKeyPrefix;
	if (_param.referenceLocation() == Loc::Memory
		&& memoryUsesBlob(_tm.map(_param.type())))
		return RefParamPassing::BlobOffset;
	return RefParamPassing::Value;
}

/// The wtype the param travels as, per its classification.
inline awst::WType const* refParamWType(
	RefParamPassing _passing,
	TypeMapper& _tm,
	solidity::frontend::VariableDeclaration const& _param)
{
	switch (_passing)
	{
	case RefParamPassing::SlotHandle: return awst::WType::biguintType();
	case RefParamPassing::BoxKeyPrefix: return awst::WType::bytesType();
	case RefParamPassing::BlobOffset: return awst::WType::uint64Type();
	case RefParamPassing::Value: break;
	}
	return _tm.map(_param.type());
}


/// Memory-ref types whose CALLEE mutations must thread back to the caller
/// (Solidity passes memory by reference; our translation copies at the
/// boundary): non-bytes arrays and structs. The SAME predicate gates the
/// callee-side return augmentation (AWSTBuilder for library/free,
/// FunctionBuilder for internal methods) and the caller-side unpack
/// (SolInternalCall) — previously three verbatim lambda copies.
inline bool isMemoryRefWriteBackType(solidity::frontend::Type const* _t)
{
	if (auto const* arr = dynamic_cast<solidity::frontend::ArrayType const*>(_t))
		return !arr->isByteArrayOrString();
	return dynamic_cast<solidity::frontend::StructType const*>(_t) != nullptr;
}

} // namespace puyasol::builder
