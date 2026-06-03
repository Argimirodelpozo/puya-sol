#pragma once

#include "builder/sol-eb/ContractContext.h"
#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <memory>
#include <optional>

namespace puyasol::builder::eb
{

/// Recognises calls to the `AVM` library (puya-sol bundled stdlib at
/// tokens/AVM.sol) and replaces them with the corresponding AVM-native
/// AWST nodes — `asset_holding_get` / `asset_params_get` intrinsics for
/// reads, `acfg` / `axfer` inner transactions for asset creation and
/// clawback transfers.
///
/// Designed to short-circuit *before* the regular library-call resolver
/// in `CallResolver::resolveFromMemberAccess`, so the AVM library's
/// stub bodies never get translated as regular subroutines.
class AsaIntrinsics
{
public:
	/// Try to handle `<base>.<member>(...)`; returns a built AWST
	/// expression iff `<base>` is the AVM library and `<member>` is a
	/// recognised intrinsic. Otherwise nullopt — caller falls through
	/// to the generic resolver.
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

	// Crypto / Group / Txn / Global library dispatchers — each switches
	// on _method name and returns an AWST expression for the matched op.
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

	// AVM scratch space (AVM.sol library Scratch): store/loadSelf/load ->
	// stores/loads/gloadss. Group-scoped ephemeral storage for flash deltas.
	static std::optional<std::shared_ptr<awst::Expression>> dispatchScratch(
		ContractContext& _ctx,
		std::string const& _method,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
