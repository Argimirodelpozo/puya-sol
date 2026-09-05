#pragma once

namespace solidity::frontend
{
class ContractDefinition;
class FunctionDefinition;
class FunctionCall;
}

namespace puyasol::builder
{

/// Resolve solc's static/virtual/super lookup for a reference-preserving call.
/// External ABI calls and unresolved function pointers return nullptr.
solidity::frontend::FunctionDefinition const* resolveReferenceCallTarget(
	solidity::frontend::ContractDefinition const* mostDerived,
	solidity::frontend::FunctionDefinition const* caller,
	solidity::frontend::FunctionCall const& call);

} // namespace puyasol::builder
