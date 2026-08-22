#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include "builder/sol-types/SolcFwd.h"

namespace puyasol::builder::eb
{

/// Instance builder for Solidity bool type.
///
/// Handles:
///   - binary_op: And (&&), Or (||), BitAnd (&), BitOr (|) on bools
///   - compare: Eq (==), Ne (!=)
///   - unary_op: LogicalNot (!)
///   - bool_eval: passthrough / negate
class SolBoolBuilder: public InstanceBuilder
{
public:
	SolBoolBuilder(ContractContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr))
	{
	}

	solidity::frontend::Type const* solType() const override;

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
};

} // namespace puyasol::builder::eb
