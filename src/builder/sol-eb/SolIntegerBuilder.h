#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Builder for Solidity integer types (uint8..uint256, int8..int256).
/// binary_op: uint64/biguint arithmetic; setbit-based shifts; square-and-multiply exp;
///   wrapping sub; unchecked wrapping. compare: XOR-sign-bit for signed ordering.
///   Overflow check for narrow types; mixed-width promotion.
/// Fields: m_bits (8..256), m_signed, m_isBigUInt (bits>64).
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

	// Biguint promotion is the shared eb::promoteToBiguint (BigUIntMathHelpers).

	/// Emit overflow check for narrow integer types.
	/// Adds assert(result <= max) to prePendingStatements.
	/// Returns the (possibly temp-var-wrapped) result expression.
	std::shared_ptr<awst::Expression> emitOverflowCheck(
		std::shared_ptr<awst::Expression> _result,
		BuilderBinaryOp _op,
		awst::SourceLocation const& _loc);

};

} // namespace puyasol::builder::eb
