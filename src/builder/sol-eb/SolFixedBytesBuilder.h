#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity fixed-size bytes types (bytes1..bytes32).
///
/// Handles:
///   - binary_op: BitOr (|), BitXor (^), BitAnd (&) → BytesBinaryOperation
///   - compare: Eq/Ne → BytesComparisonExpression, Lt/Gt/etc. → b</b>/b<=/b>=
///   - bool_eval: bytes != zero
class SolFixedBytesBuilder: public InstanceBuilder
{
public:
	SolFixedBytesBuilder(
		BuilderContext& _ctx,
		solidity::frontend::FixedBytesType const* _bytesType,
		std::shared_ptr<awst::Expression> _expr);

	solidity::frontend::Type const* solType() const override { return m_bytesType; }

	std::unique_ptr<InstanceBuilder> binary_op(
		InstanceBuilder& _other, BuilderBinaryOp _op,
		awst::SourceLocation const& _loc, bool _reverse = false) override;

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

	unsigned numBytes() const { return m_numBytes; }

private:
	solidity::frontend::FixedBytesType const* m_bytesType;
	unsigned m_numBytes;
};

} // namespace puyasol::builder::eb
