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

};

} // namespace puyasol::builder::eb
