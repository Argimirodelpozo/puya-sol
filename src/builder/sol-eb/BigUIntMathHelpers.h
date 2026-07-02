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

/// Promote a scalar expression to biguint — THE canonical value-domain promotion
/// (was copy-pasted 3x: SolIntegerBuilder, SolBinaryOperation, and here).
/// Integer constants become biguint constants directly (avoids itob chains);
/// account/bytes reinterpret through bytes→biguint (itob is uint64-only and
/// would truncate >8 bytes); uint64 goes itob→biguint. Already-biguint returns
/// unchanged. NB: AssemblyBuilder::ensureBiguint stays SEPARATE by design — it
/// carries strict-assembly aggregate-pointer semantics on top of this.
std::shared_ptr<awst::Expression> promoteToBiguint(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc);

/// Coerce a SHIFT AMOUNT to uint64 without truncating huge biguint amounts.
/// implicitNumericCast(biguint→uint64) extracts the LOW 64 BITS — safe for
/// values known < 2^64, but a shift amount is user data: `x >> 2^128` took
/// amount mod 2^64 = 0 and shifted by nothing, where EVM saturates for ANY
/// amount >= 256 (shl/shr → 0, sar → 0/-1). Clamp at the biguint level:
/// amount >= 256 → 256, which every downstream shift helper saturates on;
/// amount < 256 → the low-64 extract is exact. uint64-typed amounts pass
/// through unchanged (downstream >=256 guards already handle them).
std::shared_ptr<awst::Expression> shiftAmountToUint64(
	std::shared_ptr<awst::Expression> _amount,
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

/// Signed intN div/mod — the SINGLE implementation shared by direct `a/b`
/// (SolBinaryOperation) and compound `x/=b` (SolIntegerBuilder). Computes on the
/// absolute values then re-applies the sign (dividend sign for mod, XOR of signs
/// for div), emits the `intN.min / -1` overflow guard (div + checked only), and
/// narrows the result to the result type's native width. Self-contained: the
/// guard rides in a comma expression, so it composes anywhere (no prePending).
///
/// PRECONDITION: `_left` / `_right` are canonical 256-bit two's-complement biguint
/// values — callers sign-extend each operand from ITS OWN width first
/// (`TypeCoercion::signExtendToUint256`). `_resultBits` is the result type width
/// (the guard's INT_MIN boundary + the final narrow: <=64 → uint64, else biguint).
/// `_checked` gates the div-overflow guard (division by zero always traps in the
/// biguint FloorDiv/Mod opcode, matching EVM, so it needs no explicit assert).
std::shared_ptr<awst::Expression> buildSignedModDiv(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	BuilderBinaryOp _op,
	unsigned _resultBits,
	bool _checked,
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
