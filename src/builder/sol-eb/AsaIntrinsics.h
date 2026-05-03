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
	static std::shared_ptr<awst::Expression> handleAsaCreate(
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
};

} // namespace puyasol::builder::eb
