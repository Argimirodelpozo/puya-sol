#pragma once

/// @file Arc4Defaults.h
/// ARC4 type analysis + default-value helpers — pure WType→bytes/size
/// reasoning, no expression building (except makeZeroBytesRuntime /
/// prependArc4LengthHeader). Extracted from TypeCoercion.cpp as a
/// cohesive leaf cluster; callers (StorageMapper, SolAssignment*,
/// SolIndexAccess, SolNewExpression, SolVariableDeclaration,
/// ApprovalProgramBuilder, FunctionBuilder, AbiDecode) use these directly.

#include "awst/Node.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace puyasol::builder
{

/// True for every WType that is already an ARC4 wire value. Centralises the
/// classification so bool/tuple/fixed-point additions cannot fall through a
/// container-specific kind list.
bool isArc4EncodedType(awst::WType const* _type);

/// Runtime `bzero(_n)` cast to `_targetType`; avoids baking large zero
/// regions as pushbytes constants (puya ~4KB limit).
std::shared_ptr<awst::Expression> makeZeroBytesRuntime(
	int _n,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc);

/// Prepend a 2-byte big-endian element-count header (ARC4StaticArray → DynamicArray).
std::shared_ptr<awst::Expression> prependArc4LengthHeader(
	std::shared_ptr<awst::Expression> _expr,
	int64_t _length,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc);

/// True if T's ARC4 encoding contains any variable-length component
/// (DynamicArray, dynamic Bytes, or any container of such).
bool arc4IsDynamic(awst::WType const* _type);

/// ARC4 zero/empty encoding for T. Returns std::nullopt when not statically
/// computable (e.g. unsized Bytes). Used to initialise box-stored static
/// arrays of dynamic-element types so splice writes see a valid head/tail layout.
std::optional<std::vector<uint8_t>> arc4DefaultEncoding(awst::WType const* _type);

/// Compute the fixed encoded byte size of an ARC4 type.
/// Returns 0 for variable-length types.
int computeEncodedElementSize(awst::WType const* _type);

/// Single control point for "this memory aggregate lives in the scratch
/// blob/region model (a uint64 (region,offset) pointer) rather than as an ARC4
/// value". Currently true when the statically encoded size exceeds one 4-KiB
/// memory slot. Every blob-vs-value threshold site funnels through here so the
/// rule stays consistent; see the implementation for the planned alias-model
/// extension and its prerequisites.
bool memoryUsesBlob(awst::WType const* _type);

} // namespace puyasol::builder
