#pragma once

#include "builder/context/ContractContext.h"
#include "awst/Node.h"

#include <libsolidity/ast/ASTForward.h>

#include <memory>
#include <optional>

namespace puyasol::builder::eb
{

struct Intrinsic;

/// Intercepts calls to the libraries in `libs/AVM.sol` and maps them to
/// AVM-native AWST. Short-circuits before CallResolver so fail-fast Solidity
/// bodies are not used as runtime subroutines.
class AsaIntrinsics
{
public:
	/// Validate the canonical source unit, library and resolved signature once
	/// during program analysis. Call sites use the resulting declaration IDs.
	static Intrinsic const* descriptor(solidity::frontend::FunctionDefinition const& _function);
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

	static std::shared_ptr<awst::Expression> handleAsaTransfer(
		ContractContext& _ctx,
		std::vector<std::shared_ptr<awst::Expression>>& _args,
		awst::SourceLocation const& _loc);

};

} // namespace puyasol::builder::eb
