#pragma once

#include "awst/Node.h"

#include <libsolidity/ast/ASTForward.h>

#include <string>
#include <vector>

namespace solidity::frontend
{
class FunctionType;
}

namespace puyasol::builder
{
class TypeMapper;
namespace eb { class ContractContext; }

/// One explicit transport-boundary mapping. `arc4Signature` is the identity
/// consumed by the AVM router; `soliditySelector` is the keccak-derived value
/// observable through Solidity expressions.
struct SelectorRoute
{
	std::string arc4Signature;
	std::vector<uint8_t> soliditySelector;
};

/// Central policy for the opt-in `--evm-selectors` mode.
///
/// ARC-4 routing remains unchanged. Only Solidity-visible selector values use
/// solc/EVM identities, and transaction selectors are translated explicitly
/// when they cross back into Solidity as msg.sig or synthetic calldata.
class SelectorSemantics
{
public:
	static bool enabled(TypeMapper const& _typeMapper);

	/// Function/error selector. In compatibility mode this is the ARC-4 method
	/// constant for `_arc4Signature`; in EVM-selector mode solc's external
	/// identifier is used.
	static std::shared_ptr<awst::Expression> functionSelector(
		eb::ContractContext& _ctx,
		solidity::frontend::FunctionType const& _function,
		std::string const& _arc4Signature,
		awst::SourceLocation const& _loc);

	/// Signature-only form for abi.encodeWithSignature and fallbacks where no
	/// FunctionType survives. solc's canonical keccak selector is still used in
	/// EVM-selector mode.
	static std::shared_ptr<awst::Expression> signatureSelector(
		eb::ContractContext& _ctx,
		std::string const& _signature,
		awst::SourceLocation const& _loc);

	/// Full bytes32 event selector. Event emission continues using ARC-28; this
	/// helper is only for Solidity's observable `Event.selector` value.
	static std::shared_ptr<awst::Expression> eventSelector(
		eb::ContractContext& _ctx,
		std::string const& _signature,
		awst::WType const* _targetType,
		awst::SourceLocation const& _loc);

	/// All public function/getter selector mappings for the current contract.
	static std::vector<SelectorRoute> routes(eb::ContractContext& _ctx);

	/// Translate an ARC-4 ApplicationArgs[0] value to its Solidity selector.
	/// Unknown values pass through unchanged so fallback calldata retains its
	/// original first four bytes.
	static std::shared_ptr<awst::Expression> runtimeSelector(
		eb::ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _routingSelector,
		awst::SourceLocation const& _loc);

	/// Context-free form used by AssemblyBuilder after SolInlineAssembly passes
	/// the current contract's precomputed route map.
	static std::shared_ptr<awst::Expression> translateRuntimeSelector(
		std::shared_ptr<awst::Expression> _routingSelector,
		std::vector<SelectorRoute> const& _routes,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder
