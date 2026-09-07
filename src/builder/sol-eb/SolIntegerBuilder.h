#pragma once

#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-types/SolIntType.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Builder for Solidity integer types (uint8..uint256, int8..int256).
/// binary_op: uint64/biguint arithmetic; setbit-based shifts; square-and-multiply exp;
///   wrapping sub; unchecked wrapping. compare: XOR-sign-bit for signed ordering.
///   Overflow check for narrow types; mixed-width promotion.
/// The integer descriptor is the SolIntType carrier (m_int); the biguint-backed
/// question is DERIVED from it (bits>64), never stored separately.
class SolIntegerBuilder: public InstanceBuilder
{
public:
	SolIntegerBuilder(
		ContractContext& _ctx,
		solidity::frontend::IntegerType const* _intType,
		std::shared_ptr<awst::Expression> _expr);

	solidity::frontend::Type const* solType() const override { return m_intType; }

	std::unique_ptr<InstanceBuilder> binary_op(
		InstanceBuilder& _other, BuilderBinaryOp _op,
		awst::SourceLocation const& _loc, bool _reverse = false) override;

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> unary_op(
		BuilderUnaryOp _op, awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

	unsigned bits() const { return m_int.bits; }
	bool isSigned() const { return m_int.isSigned; }
	bool isBigUInt() const { return m_int.biguintBacked(); }

private:
	solidity::frontend::IntegerType const* m_intType;
	SolIntType m_int;

	/// Create a new SolIntegerBuilder wrapping the given expression,
	/// preserving this builder's Solidity type info.
	std::unique_ptr<SolIntegerBuilder> wrap(std::shared_ptr<awst::Expression> _expr) const;

	// Biguint promotion is the shared eb::promoteToBiguint (BigUIntMathHelpers).

	/// Emit overflow check for narrow integer types.
	/// Adds assert(result <= max) to the current pre-effect frame.
	/// Returns the (possibly temp-var-wrapped) result expression.
	std::shared_ptr<awst::Expression> emitOverflowCheck(
		std::shared_ptr<awst::Expression> _result,
		BuilderBinaryOp _op,
		awst::SourceLocation const& _loc);

	// ── binary_op rungs (SolIntegerBuilder.cpp), one per operator family,
	// dispatched in binary_op's check order. Operands arrive resolved (and
	// already swapped for `_reverse`); the biguint rungs get `_lhs` promoted
	// to biguint — and `_rhs` too, except the shift rung (amount stays
	// uint64). Every rung ends in wrap(). ──────────────────────────────

	/// biguint `<<` / `>>`: clamped amount, SAR for signed `>>`, width mask
	/// for unsigned `<<`, narrow back to uint64 for sub-word types.
	std::unique_ptr<InstanceBuilder> buildBigUIntShiftOp(
		BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
		std::shared_ptr<awst::Expression> _rhs, awst::SourceLocation const& _loc);
	/// biguint `-` (unsigned, or the uint64 unchecked-underflow route):
	/// wrapping subtract + unchecked width mask + uint64 narrowing.
	std::unique_ptr<InstanceBuilder> buildBigUIntSubOp(
		std::shared_ptr<awst::Expression> _lhs, std::shared_ptr<awst::Expression> _rhs,
		awst::SourceLocation const& _loc);
	/// biguint `**`: square-and-multiply + unchecked width mask.
	std::unique_ptr<InstanceBuilder> buildBigUIntPowOp(
		std::shared_ptr<awst::Expression> _lhs, std::shared_ptr<awst::Expression> _rhs,
		awst::SourceLocation const& _loc);
	/// Signed `/` `%`: sign-extend each operand from ITS OWN width
	/// (`_lhsBits` / `_rhsBits`), then the shared buildSignedModDiv.
	std::unique_ptr<InstanceBuilder> buildSignedModDivOp(
		BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
		std::shared_ptr<awst::Expression> _rhs, unsigned _lhsBits, unsigned _rhsBits,
		awst::SourceLocation const& _loc);
	/// Remaining biguint ops (add / mult / div / mod / bitwise) as a
	/// BigUIntBinaryOperation + unchecked Add/Mult wrap + overflow check.
	std::unique_ptr<InstanceBuilder> buildBigUIntArithBitwiseOp(
		BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
		std::shared_ptr<awst::Expression> _rhs, awst::SourceLocation const& _loc);
	/// uint64 (bits==64) UNCHECKED Add/Mult: the AVM opcodes panic on
	/// overflow, so wide-compute via biguint mod 2^64 and narrow back.
	std::unique_ptr<InstanceBuilder> buildUInt64WrappingAddMult(
		BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
		std::shared_ptr<awst::Expression> _rhs, awst::SourceLocation const& _loc);
	/// uint64 `**` on the operation node built by the table rung (`_e` has
	/// left/right set): unchecked → biguint exp mod 2^bits narrowed to uint64;
	/// checked → native `exp` with the `0**0 = 1` guard + overflow check.
	std::unique_ptr<InstanceBuilder> buildUInt64PowOp(
		std::shared_ptr<awst::UInt64BinaryOperation> _e, awst::SourceLocation const& _loc);
	/// The uint64 opcode table (add / sub with its unchecked sub-word wrap /
	/// mult / div / mod / shifts / bitwise; `**` delegates to buildUInt64PowOp)
	/// + unchecked sub-word mask + overflow check.
	std::unique_ptr<InstanceBuilder> buildUInt64ArithBitwiseOp(
		BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
		std::shared_ptr<awst::Expression> _rhs, awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
