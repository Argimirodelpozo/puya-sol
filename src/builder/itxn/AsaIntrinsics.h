#pragma once

#include "builder/sol-eb/ContractContext.h"
#include "awst/Node.h"

#include <libsolidity/ast/ASTForward.h>

#include <memory>
#include <optional>

namespace puyasol::builder::eb
{

/// Intercepts calls to the libraries in `libs/AVM.sol` and maps them to
/// AVM-native AWST. Short-circuits before CallResolver so fail-fast Solidity
/// bodies are not used as runtime subroutines.
class AsaIntrinsics
{
public:
	/// True only for the canonical `Bits.bitlen(uint256)` declaration in
	/// `libs/AVM.sol`. Its body is a fail-fast Solidity stub; direct calls are
	/// lowered to the native AVM `bitlen` opcode.
	static bool isBitsBitlenFacade(
		solidity::frontend::FunctionDefinition const& _function);

	/// Try to handle `<base>.<member>(...)`; returns built expression iff
	/// base is an AVM stdlib library and member is a known intrinsic.
	/// Returns nullopt to fall through to the generic resolver.
	static std::optional<std::shared_ptr<awst::Expression>> tryHandleCall(
		ContractContext& _ctx,
		solidity::frontend::MemberAccess const& _memberAccess,
		solidity::frontend::FunctionCall const& _call,
		awst::SourceLocation const& _loc);

private:
	// ASA handlers
	static std::shared_ptr<awst::Expression> handleAsaCreate(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> handleAsaDestroy(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> handleAsaOptIn(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> handleAsaFreeze(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> handleAsaBalance(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> handleAsaTotalSupply(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> handleAsaDecimals(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> handleAsaUnitName(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> handleAsaName(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::shared_ptr<awst::Expression> handleAsaTransfer(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	// Crypto / Group / Txn / Global / Scratch library dispatchers.
	static std::optional<std::shared_ptr<awst::Expression>> dispatchCrypto(
		ContractContext& _ctx,
		std::string const& _method,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::optional<std::shared_ptr<awst::Expression>> dispatchGroup(
		ContractContext& _ctx,
		std::string const& _method,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::optional<std::shared_ptr<awst::Expression>> dispatchTxn(
		ContractContext& _ctx,
		std::string const& _method,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::optional<std::shared_ptr<awst::Expression>> dispatchGlobal(
		ContractContext& _ctx,
		std::string const& _method,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	static std::optional<std::shared_ptr<awst::Expression>> dispatchBits(
		ContractContext& _ctx,
		std::string const& _method,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

	// AVM scratch space (AVM.sol Scratch): store/loadSelf/load → stores/loads/gloadss.
	static std::optional<std::shared_ptr<awst::Expression>> dispatchScratch(
		ContractContext& _ctx,
		std::string const& _method,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
