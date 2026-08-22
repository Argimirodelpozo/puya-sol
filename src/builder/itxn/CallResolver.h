#pragma once

#include "builder/sol-ast/Context.h"
#include "builder/sol-eb/ContractContext.h"
#include "awst/Node.h"

#include <libsolidity/ast/ASTForward.h>
#include "builder/sol-types/SolcFwd.h"

#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace puyasol::builder::eb
{

/// Result of resolving a function call target.
struct ResolvedCall
{
	awst::SubroutineTarget target; ///< Same variant as SubroutineCallExpression::target.
	solidity::frontend::FunctionDefinition const* funcDef = nullptr; ///< May be null.
	bool isUsingForCall = false;    ///< Receiver prepended as first arg.
	bool isSuperCall = false;
	bool isBaseInternalCall = false;
	bool isExternalCall = false;    ///< Needs inner txn.
};

enum class CallTransport
{
	Internal,
	External,
};

/// Source-level call classification computed from solc annotations. Argument
/// binding and target emission consume this shared plan instead of each call
/// builder re-inspecting the callee syntax.
struct CallPlan
{
	CallTransport transport = CallTransport::Internal;
	solidity::frontend::FunctionType const* functionType = nullptr;
	solidity::frontend::Expression const* callee = nullptr;
	solidity::frontend::FunctionDefinition const* declaration = nullptr;
	bool isSelfCall = false;
	bool isFunctionPointer = false;
};

/// Resolves function call targets (library, free function, super, base
/// internal, external, or regular instance method) from Solidity AST.
class CallResolver
{
public:
	/// Classify an already type-checked call as an internal/subroutine path or
	/// an external inner-transaction path.
	static CallPlan plan(solidity::frontend::FunctionCall const& _call);

	/// Try to resolve a function call from an Identifier callee; nullopt on failure.
	static std::optional<ResolvedCall> resolveFromIdentifier(
		ContractContext& _ctx,
		solidity::frontend::Identifier const& _ident,
		std::string const& _resolvedName);

	/// Try to resolve a function call from a MemberAccess callee; nullopt on failure.
	static std::optional<ResolvedCall> resolveFromMemberAccess(
		ContractContext& _ctx,
		sol_ast::Context& _scope,
		solidity::frontend::MemberAccess const& _memberAccess,
		std::string const& _resolvedName,
		size_t _argCount);

	/// Returns "name(paramTypes)" if overloaded, else "name".
	static std::string resolveMethodName(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const& _func);

	/// Apply solc's virtual lookup for the contract currently being lowered.
	/// Non-virtual, free, library, constructor, and non-contract functions are
	/// returned unchanged.
	static solidity::frontend::FunctionDefinition const& resolveVirtualTarget(
		ContractContext const& _ctx,
		solidity::frontend::FunctionDefinition const& _func);

private:
	/// Resolve library or free function by AST ID and name.
	static bool tryResolveLibraryOrFree(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const* _funcDef,
		ResolvedCall& _result);
};

} // namespace puyasol::builder::eb
