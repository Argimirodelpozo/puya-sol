#pragma once

/// @file Arc4ArrayWidening.h
/// ARC4 integer-array shape widening helpers — convert
/// `arc4.static_array<arc4.intM, K>` / `arc4.dynamic_array<arc4.intM>`
/// to the same-shape array of `arc4.intN` where N > M. Used at
/// assignment sites where Solidity allows an implicit widening of the
/// element type but the ARC4 wire encoding requires per-element padding
/// (zero-extend for unsigned, sign-extend for signed).
///
/// Extracted from `TypeCoercion.cpp` — these three functions form a
/// cohesive leaf cluster called only from `SolAssignment.cpp`. No
/// other TypeCoercion entry-point depends on them.

#include "awst/Node.h"

#include <functional>
#include <memory>

namespace puyasol::builder
{

/// Narrow a `uint64` stack value to `arc4.uintN` where N < 64. Puya has
/// no codec for this direction; we emit `extract3(itob(v), 8-N/8, N/8)`
/// → ReinterpretCast<arc4.uintN>. Two's complement makes this correct
/// for signed targets too: uint64(-3) = 0xFF…FFFD; low byte 0xFD = int8(-3).
/// `value` is evaluated exactly once (the bytes flow through `itob` only).
/// Returns nullptr if the shapes don't match (caller falls back to ARC4Encode).
std::shared_ptr<awst::Expression> tryNarrowUInt64ToArc4UIntN(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc);

/// Widen each element of `arc4.static_array<arc4.intM, K>` to
/// `arc4.static_array<arc4.intN, K>` (M < N), sign- or zero-extending
/// based on the alias prefix (`intM` vs `uintM`). `_mkSourceBytes` is
/// called K times to obtain fresh `bytes`-typed expressions for the
/// source; pass a closure that returns a VarExpression to a pre-pinned
/// temp if the original `value` has side effects.
/// Returns nullptr if the shapes don't match (caller falls back).
std::shared_ptr<awst::Expression> tryWidenArc4StaticArrayInt(
	awst::WType const* _sourceType,
	awst::WType const* _targetType,
	std::function<std::shared_ptr<awst::Expression>()> _mkSourceBytes,
	awst::SourceLocation const& _loc);

/// Widen each element of `arc4.dynamic_array<arc4.intM>` to
/// `arc4.dynamic_array<arc4.intN>` (M < N) at runtime via a WhileLoop.
/// The length prefix (2-byte uint16) is carried through; each element
/// gets sign- or zero-extended per the alias. `_mkSourceBytes` must
/// return a fresh, side-effect-free expression for the source bytes
/// each call (the helper reads it multiple times — once for the length
/// header and once per iteration). `_emit` is called to attach the
/// setup assignments and loop statement to the caller's pre-pending
/// statement list. Returns nullptr if the shapes don't match.
std::shared_ptr<awst::Expression> tryWidenArc4DynamicArrayInt(
	awst::WType const* _sourceType,
	awst::WType const* _targetType,
	std::function<std::shared_ptr<awst::Expression>()> _mkSourceBytes,
	std::function<void(std::shared_ptr<awst::Statement>)> _emit,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder
