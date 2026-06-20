#pragma once

/// @file BigUIntMathHelpers.h
/// EVM-compatible 256-bit biguint math helpers: setbit-based shifts,
/// square-and-multiply exp, wrapping subtraction, mod-2^256, signed mod/div.
/// Extracted from SolIntegerBuilder so any builder can reuse without the
/// SolIntegerBuilder instance state. Stateful helpers take ContractContext& to
/// append pre-pending statements; isUnchecked is a bool to avoid scope threading.

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

/// Arithmetic shift right (SAR) for 256-bit two's-complement: fills sign bit,
/// clamps shift to 255 (saturates to 0 / all-ones). Logical >> (buildBigUIntShift)
/// would zero-fill negatives. `_value` must be canonical 256-bit two's complement.
std::shared_ptr<awst::Expression> buildBigUIntArithmeticShiftRight(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _shiftAmt,
	awst::SourceLocation const& _loc);

/// Square-and-multiply exponentiation; appends init+loop to prePendingStatements.
/// Unchecked mode wraps intermediate products mod 2^256.
std::shared_ptr<awst::Expression> buildBigUIntExp(
	ContractContext& _ctx,
	bool _isUnchecked,
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _exp,
	awst::SourceLocation const& _loc);

/// `(a + 2^256 - b) % 2^256`. In checked mode also prepends `assert(a >= b)`.
std::shared_ptr<awst::Expression> buildWrappingSubtract(
	ContractContext& _ctx,
	bool _isUnchecked,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::SourceLocation const& _loc);

/// Signed biguint mod/div: abs-value arithmetic then re-apply sign
/// (dividend sign for mod, XOR of signs for div).
std::shared_ptr<awst::Expression> buildSignedModDiv(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	BuilderBinaryOp _op,
	awst::SourceLocation const& _loc);

/// Signed integer add/sub/mul (_op ∈ {Add,Sub,Mult}): mod-2^N two's-complement result +
/// (checked) the signed-overflow assert + sub-256 canonicalisation to 256-bit two's complement
/// (≤64-bit results truncate to uint64). The signed counterpart of the raw biguint ops — shared by
/// SolBinaryOperation (`a+b`) and the compound path (`x+=d`, via SolIntegerBuilder::binary_op),
/// which previously hit the UNSIGNED biguint add and mishandled signed compound assignment.
std::shared_ptr<awst::Expression> buildSignedArithmetic(
	ContractContext& _ctx,
	bool _isUnchecked,
	BuilderBinaryOp _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	unsigned _bits,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder::eb
