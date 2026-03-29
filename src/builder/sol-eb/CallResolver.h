#pragma once

#include "builder/sol-eb/BuilderContext.h"
#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace puyasol::builder::eb
{

/// Result of resolving a function call target.
struct ResolvedCall
{
	/// The call target — uses the same variant as SubroutineCallExpression::target.
	awst::SubroutineTarget target;

	/// The resolved FunctionDefinition (for argument type coercion). May be null.
	solidity::frontend::FunctionDefinition const* funcDef = nullptr;

	/// Whether this is a using-for call (receiver is first argument).
	bool isUsingForCall = false;

	/// Whether this is a super call.
	bool isSuperCall = false;

	/// Whether this is a base internal call (e.g., BaseContract.method()).
	bool isBaseInternalCall = false;

	/// Whether this is an external interface/contract call (needs inner txn).
	bool isExternalCall = false;
};

/// Resolves function call targets from Solidity AST.
///
/// Extracts the logic for determining WHETHER a call is to a library,
/// free function, super method, base internal method, external interface,
/// or regular instance method — and WHAT the subroutine ID or method name is.
class CallResolver
{
public:
	/// Try to resolve a function call from an Identifier callee.
	/// Returns nullopt if resolution fails.
	static std::optional<ResolvedCall> resolveFromIdentifier(
		BuilderContext& _ctx,
		solidity::frontend::Identifier const& _ident,
		std::string const& _resolvedName);

	/// Try to resolve a function call from a MemberAccess callee.
	/// Returns nullopt if resolution fails.
	static std::optional<ResolvedCall> resolveFromMemberAccess(
		BuilderContext& _ctx,
		solidity::frontend::MemberAccess const& _memberAccess,
		std::string const& _resolvedName,
		size_t _argCount);

private:
	/// Try library/free function resolution by AST ID and name.
	static bool tryResolveLibraryOrFree(
		BuilderContext& _ctx,
		solidity::frontend::FunctionDefinition const* _funcDef,
		ResolvedCall& _result);
};

} // namespace puyasol::builder::eb
