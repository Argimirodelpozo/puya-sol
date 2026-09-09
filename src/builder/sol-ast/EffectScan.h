#pragma once

namespace solidity::frontend { class Expression; }

namespace puyasol::builder
{
namespace eb { class ContractContext; }

/// Source effects that queued AWST effects cannot see, including mutation
/// through shared memory in a pure/view internal call.
class EffectScan
{
public:
	static bool mayWrite(solidity::frontend::Expression const& expression,
		eb::ContractContext& context);
};

} // namespace puyasol::builder
