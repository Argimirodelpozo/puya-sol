#pragma once

/// @file Arc4Defaults.h
/// ARC4 type analysis + default-value helpers — pure WType→bytes/size
/// reasoning, no expression building (except the two tiny
/// `makeZeroBytesRuntime` / `prependArc4LengthHeader` wrappers used by
/// the cluster + by `TypeCoercion::makeDefaultValue` /
/// `coerceForAssignment`).
///
/// Extracted from `TypeCoercion.cpp` — these five functions form a
/// cohesive leaf cluster. None of them call any non-cluster
/// `TypeCoercion::` entry-point; all external callers
/// (StorageMapper, SolAssignment*, SolIndexAccess, SolNewExpression,
/// SolVariableDeclaration, ApprovalProgramBuilder, FunctionBuilder,
/// AbiDecode) now use these free functions directly.

#include "awst/Node.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace puyasol::builder
{

/// Emit `bzero(_n)` wrapped in a ReinterpretCast to _targetType so the
/// zero region is allocated at runtime instead of baked into a pushbytes
/// constant. Used for default values of large ARC4 static arrays and
/// fixed-size byte arrays, whose bytecode-inlined form would exceed
/// puya's ~4KB bytes constant limit.
std::shared_ptr<awst::Expression> makeZeroBytesRuntime(
	int _n,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc);

/// Convert a fixed-size ARC4 static array to a dynamic ARC4 array by
/// prepending a 2-byte big-endian length header with the statically
/// known element count.
std::shared_ptr<awst::Expression> prependArc4LengthHeader(
	std::shared_ptr<awst::Expression> _expr,
	int64_t _length,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc);

/// True if the ARC4 encoding of T contains any dynamic (variable-length)
/// component (DynamicArray, dynamic-length Bytes, or any container of
/// such). Static arrays/structs are themselves dynamic if any element/field
/// is dynamic.
bool arc4IsDynamic(awst::WType const* _type);

/// Default ARC4 encoding (the byte sequence representing the zero/empty
/// value of T). Returns std::nullopt for types whose default encoding is
/// not statically computable (e.g. Bytes with no fixed length used outside
/// a structured ARC4 context). Used to initialise box-stored static arrays
/// of dynamic-element types so that subsequent splice writes operate on a
/// valid ARC4 head/tail layout instead of all-zero garbage.
std::optional<std::vector<uint8_t>> arc4DefaultEncoding(awst::WType const* _type);

/// Compute the fixed encoded byte size of an ARC4 type.
/// Returns 0 for variable-length types.
int computeEncodedElementSize(awst::WType const* _type);

} // namespace puyasol::builder
