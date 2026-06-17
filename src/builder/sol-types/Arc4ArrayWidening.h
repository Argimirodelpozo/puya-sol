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

/// Narrow a `uint64` to `arc4.uintN` (N < 64) via `extract3(itob(v), 8-N/8, N/8)`.
/// Two's complement makes this correct for signed targets too (e.g. uint64(-3)
/// low byte 0xFD = int8(-3)). Returns nullptr if shapes don't match.
std::shared_ptr<awst::Expression> tryNarrowUInt64ToArc4UIntN(
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc);

/// Widen each element of `arc4.static_array<arc4.intM, K>` to
/// `arc4.static_array<arc4.intN, K>` (M < N), sign/zero-extending per alias
/// prefix (`intM` vs `uintM`). `_mkSourceBytes` is called K times; pass a
/// closure returning a VarExpression to a pre-pinned temp for side-effect safety.
/// Returns nullptr if shapes don't match.
std::shared_ptr<awst::Expression> tryWidenArc4StaticArrayInt(
	awst::WType const* _sourceType,
	awst::WType const* _targetType,
	std::function<std::shared_ptr<awst::Expression>()> _mkSourceBytes,
	awst::SourceLocation const& _loc);

/// Widen each element of `arc4.dynamic_array<arc4.intM>` to
/// `arc4.dynamic_array<arc4.intN>` (M < N) via a WhileLoop. Length prefix
/// carried through; elements sign/zero-extended per alias. `_mkSourceBytes`
/// must return a fresh side-effect-free expression each call (read for
/// length header + once per iteration). `_emit` attaches setup stmts and
/// the loop to the caller's pre-pending list. Returns nullptr if no match.
std::shared_ptr<awst::Expression> tryWidenArc4DynamicArrayInt(
	awst::WType const* _sourceType,
	awst::WType const* _targetType,
	std::function<std::shared_ptr<awst::Expression>()> _mkSourceBytes,
	std::function<void(std::shared_ptr<awst::Statement>)> _emit,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder
