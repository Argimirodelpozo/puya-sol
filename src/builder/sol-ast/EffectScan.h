#pragma once

namespace solidity::frontend { class Expression; }

namespace puyasol::builder
{
namespace eb { class ContractContext; }
namespace sol_ast { class Context; }

/// Source effects that queued AWST effects cannot see, including mutation
/// through shared memory in a pure/view internal call.
class EffectScan
{
public:
	static bool mayWrite(solidity::frontend::Expression const& expression,
		eb::ContractContext& context, sol_ast::Context const& scope);
};

} // namespace puyasol::builder
