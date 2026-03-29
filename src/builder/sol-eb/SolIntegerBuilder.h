#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity integer types (uint8..uint256, int8..int256).
///
/// This is the most important builder — it handles ALL integer operations
/// with full Solidity semantics:
///
///   - binary_op: arithmetic (uint64 or biguint), shifts (setbit trick for biguint),
///     exponentiation (square-and-multiply for biguint, 0^0 guard for uint64),
///     wrapping subtraction for biguint, unchecked block wrapping
///   - compare: unsigned NumericComparison, signed comparison via XOR with sign bit
///   - Overflow checking for narrow types (uint8 + uint8 → assert ≤ 255)
///   - Mixed-width promotion (uint64 operand promoted to biguint when needed)
///
/// The builder stores the Solidity IntegerType and derives:
///   - m_bits (8..256), m_signed (int vs uint)
///   - m_isBigUInt (bits > 64 → biguint on AVM)
///   - Target WType (uint64Type or biguintType)
class SolIntegerBuilder: public InstanceBuilder
{
public:
	SolIntegerBuilder(
		BuilderContext& _ctx,
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

	unsigned bits() const { return m_bits; }
	bool isSigned() const { return m_signed; }
	bool isBigUInt() const { return m_isBigUInt; }

private:
	solidity::frontend::IntegerType const* m_intType;
	unsigned m_bits;
	bool m_signed;
	bool m_isBigUInt;

	/// Create a new SolIntegerBuilder wrapping the given expression,
	/// preserving this builder's Solidity type info.
	std::unique_ptr<SolIntegerBuilder> wrap(std::shared_ptr<awst::Expression> _expr) const;

	/// Promote a uint64 expression to biguint.
	static std::shared_ptr<awst::Expression> promoteToBigUInt(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc);

	/// Emit overflow check for narrow integer types.
	/// Adds assert(result <= max) to prePendingStatements.
	/// Returns the (possibly temp-var-wrapped) result expression.
	std::shared_ptr<awst::Expression> emitOverflowCheck(
		std::shared_ptr<awst::Expression> _result,
		BuilderBinaryOp _op,
		awst::SourceLocation const& _loc);

	/// Build biguint shift: x * 2^n or x / 2^n using setbit(bzero(32), 255-n, 1).
	std::shared_ptr<awst::Expression> buildBigUIntShift(
		std::shared_ptr<awst::Expression> _value,
		std::shared_ptr<awst::Expression> _shiftAmt,
		bool _isLeftShift,
		awst::SourceLocation const& _loc);

	/// Build biguint exponentiation via square-and-multiply loop.
	std::shared_ptr<awst::Expression> buildBigUIntExp(
		std::shared_ptr<awst::Expression> _base,
		std::shared_ptr<awst::Expression> _exp,
		awst::SourceLocation const& _loc);

	/// Build wrapping biguint subtraction: (a + 2^256 - b) % 2^256.
	std::shared_ptr<awst::Expression> buildWrappingSubtract(
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		awst::SourceLocation const& _loc);

	/// Wrap biguint result mod 2^256 (for unchecked blocks).
	std::shared_ptr<awst::Expression> wrapMod256(
		std::shared_ptr<awst::Expression> _expr,
		awst::SourceLocation const& _loc);

	/// The 2^256 constant string.
	static constexpr char const* POW_2_256 =
		"115792089237316195423570985008687907853269984665640564039457584007913129639936";
};

} // namespace puyasol::builder::eb
