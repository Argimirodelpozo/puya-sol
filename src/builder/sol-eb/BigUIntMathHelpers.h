#pragma once

/// @file BigUIntMathHelpers.h
/// Free functions for EVM-compatible 256-bit biguint arithmetic on AVM:
/// the setbit-based shift trick, square-and-multiply exponentiation,
/// wrapping subtraction, mod-2^256 wrap, and signed mod/div via
/// absolute-value plus sign reapplication. Extracted from
/// SolIntegerBuilder to make the math reusable from any builder
/// without the SolIntegerBuilder instance state (m_bits/m_signed etc).
///
/// The stateful helpers (`buildBigUIntExp`, `buildWrappingSubtract`)
/// take the `ContractContext&` directly so they can append the loop /
/// underflow-assert pre-pending statements. `isUnchecked` is passed
/// in as a bool so callers don't need to hand over a scope reference.

#include "awst/Node.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-eb/ContractContext.h"

#include <memory>

namespace puyasol::builder::eb
{

/// Wrap a biguint expression mod 2^256 (the EVM word boundary).
std::shared_ptr<awst::Expression> wrapMod256(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc);

/// Build biguint shift: `value * 2^n` (left) or `value / 2^n` (right),
/// using the setbit(bzero(32), 255-n, 1) trick for the power.
std::shared_ptr<awst::Expression> buildBigUIntShift(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _shiftAmt,
	bool _isLeftShift,
	awst::SourceLocation const& _loc);

/// Build ARITHMETIC shift right (Solidity `>>` on a signed int = SAR) for a
/// 256-bit two's-complement value: `(v >= 2^255) ? (v/2^n | topNbitsMask)
/// : v/2^n`, with the shift clamped to 255 (any shift >= 255 saturates to 0
/// for non-negative / all-ones for negative). Logical `>>` (buildBigUIntShift)
/// sign-fills with zeros, which is wrong for negative values. `_value` must be
/// in canonical 256-bit two's-complement form.
std::shared_ptr<awst::Expression> buildBigUIntArithmeticShiftRight(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _shiftAmt,
	awst::SourceLocation const& _loc);

/// Build biguint exponentiation via square-and-multiply. Appends the
/// init assigns + while-loop to `_ctx.prePendingStatements`; returns a
/// VarExpression for the accumulator var. In unchecked mode the
/// intermediate products wrap mod 2^256.
std::shared_ptr<awst::Expression> buildBigUIntExp(
	ContractContext& _ctx,
	bool _isUnchecked,
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _exp,
	awst::SourceLocation const& _loc);

/// Build wrapping biguint subtraction: `(a + 2^256 - b) % 2^256`.
/// In checked mode also appends `assert(a >= b)` to
/// `_ctx.prePendingStatements`.
std::shared_ptr<awst::Expression> buildWrappingSubtract(
	ContractContext& _ctx,
	bool _isUnchecked,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::SourceLocation const& _loc);

/// Build signed biguint mod or div: operate on the two's-complement
/// absolute values, then apply the result sign (dividend's sign for
/// mod, XOR of operand signs for div).
std::shared_ptr<awst::Expression> buildSignedModDiv(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	BuilderBinaryOp _op,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder::eb
