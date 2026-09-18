#pragma once

#include "builder/eb/NodeBuilder.h"

#include "builder/solc/SolcFwd.h"

namespace puyasol::builder::eb
{

/// Instance builder for Solidity address/contract types.
///
/// Handles:
///   - compare: Eq/Ne → BytesComparisonExpression (address is bytes-backed)
class SolAddressBuilder: public InstanceBuilder
{
public:
	SolAddressBuilder(
		ContractContext& _ctx,
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_solType(_solType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_solType; }

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

private:
	solidity::frontend::Type const* m_solType;
};

} // namespace puyasol::builder::eb
