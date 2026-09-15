#pragma once

#include "builder/context/ContractContext.h"
#include "awst/Node.h"

#include <libsolidity/ast/ASTForward.h>
#include "builder/solc/SolcFwd.h"

#include <memory>
#include <initializer_list>
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

	/// Solc-resolved user operators are ordinary free-function calls, including
	/// host-bound targets. Both solc pipelines evaluate their operands left-first.
	static std::shared_ptr<awst::Expression> buildOperatorCall(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const& _function,
		std::initializer_list<solidity::frontend::Expression const*> _operands,
		awst::SourceLocation const& _loc);

	/// Resolve a function expression once: exact solc body plus its AWST target.
	/// Null for data/function-pointer expressions without a concrete declaration.
	static std::optional<ResolvedCall> resolveFunction(
		ContractContext& _ctx,
		solidity::frontend::Expression const& _expression);

	/// Name of an already-resolved method; does not repeat virtual lookup.
	static std::string resolveMethodName(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const& _func);

	/// Register an exact base implementation for shared internal-body emission.
	static std::string baseImplementationName(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const& _func);

private:
	/// Resolve library or free function by AST ID and name.
	static bool tryResolveLibraryOrFree(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const* _funcDef,
		ResolvedCall& _result);
};

} // namespace puyasol::builder::eb
