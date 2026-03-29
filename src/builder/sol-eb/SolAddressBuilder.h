#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity address/contract types.
///
/// Handles:
///   - compare: Eq/Ne → BytesComparisonExpression (address is bytes-backed)
///   - member_access: .code, .balance (future: .call, .staticcall, .transfer)
///   - bool_eval: addr != zero_address
class SolAddressBuilder: public InstanceBuilder
{
public:
	SolAddressBuilder(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_solType(_solType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_solType; }

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

private:
	solidity::frontend::Type const* m_solType;
};

} // namespace puyasol::builder::eb
