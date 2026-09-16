#pragma once

#include "builder/eb/NodeBuilder.h"

#include "builder/solc/SolcFwd.h"

namespace puyasol::builder::eb
{

/// Instance builder for Solidity bool type.
///
/// Handles:
///   - compare: Eq (==), Ne (!=)
///   - unary_op: LogicalNot (!)
class SolBoolBuilder: public InstanceBuilder
{
public:
	SolBoolBuilder(ContractContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr))
	{
	}

	solidity::frontend::Type const* solType() const override;

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> unary_op(
		BuilderUnaryOp _op, awst::SourceLocation const& _loc) override;

};

} // namespace puyasol::builder::eb
