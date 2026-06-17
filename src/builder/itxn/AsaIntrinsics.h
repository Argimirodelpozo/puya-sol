#pragma once

#include "builder/sol-eb/ContractContext.h"
#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <memory>
#include <optional>

namespace puyasol::builder::eb
{

/// Intercepts AVM stdlib calls (tokens/AVM.sol) and maps them to AVM-native
/// AWST: `asset_holding_get`/`asset_params_get` for reads, `acfg`/`axfer`
/// inner txns for mutations. Short-circuits before CallResolver so the
/// library stubs are never translated as regular subroutines.
class AsaIntrinsics
{
public:
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

	// AVM scratch space (AVM.sol Scratch): store/loadSelf/load → stores/loads/gloadss.
	static std::optional<std::shared_ptr<awst::Expression>> dispatchScratch(
		ContractContext& _ctx,
		std::string const& _method,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::eb
