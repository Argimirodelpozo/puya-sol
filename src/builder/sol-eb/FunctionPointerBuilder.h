#pragma once
/// @file FunctionPointerBuilder.h
/// Handles Solidity function pointer types — both internal and external.
///
/// Internal function pointers are stored as uint64 IDs. Calling one dispatches
/// through a generated __funcptr_dispatch subroutine with a switch table.
///
/// External function pointers are stored as bytes (address + selector).
/// Calling one dispatches through an inner application call.

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <map>
#include <string>
#include <vector>

namespace puyasol::builder::eb
{

/// Registry of internal function pointer targets and their IDs.
/// Built up during contract translation, then used to generate dispatch tables.
struct FuncPtrEntry
{
	int64_t astId;            // Solidity AST ID of the FunctionDefinition
	std::string name;         // Function name (for dispatch subroutine)
	unsigned id;              // Unique integer ID for this target
	solidity::frontend::FunctionType const* funcType; // Signature
};

class FunctionPointerBuilder
{
public:
	/// Map a Solidity FunctionType to the appropriate WType.
	/// Internal → uint64, External → bytes.
	static awst::WType const* mapFunctionType(
		solidity::frontend::FunctionType const* _funcType);

	/// Build an expression that represents taking the address of a function.
	/// For internal: returns IntegerConstant with the function's ID.
	/// For external: returns bytes (address + selector).
	static std::shared_ptr<awst::Expression> buildFunctionReference(
		BuilderContext& _ctx,
		solidity::frontend::FunctionDefinition const* _funcDef,
		awst::SourceLocation const& _loc);

	/// Build a call through a function pointer.
	/// For internal: calls __funcptr_dispatch(id, args...).
	/// For external: inner app call.
	static std::shared_ptr<awst::Expression> buildFunctionPointerCall(
		BuilderContext& _ctx,
		std::shared_ptr<awst::Expression> _ptrExpr,
		solidity::frontend::FunctionType const* _funcType,
		std::vector<std::shared_ptr<awst::Expression>> _args,
		awst::SourceLocation const& _loc);

	/// Register a function as a potential function pointer target.
	/// Called during contract translation for any function whose address is taken.
	static void registerTarget(
		solidity::frontend::FunctionDefinition const* _funcDef,
		solidity::frontend::FunctionType const* _funcType);

	/// Generate the __funcptr_dispatch subroutine(s) for all registered targets.
	/// Groups targets by signature (param types + return types) and generates
	/// one dispatch subroutine per signature group.
	/// Called from ContractBuilder after all methods are translated.
	static std::vector<awst::ContractMethod> generateDispatchMethods(
		std::string const& _cref,
		awst::SourceLocation const& _loc);

	/// Get the dispatch subroutine name for a given function type signature.
	static std::string dispatchName(
		solidity::frontend::FunctionType const* _funcType);

	/// Clear all registered targets (between contracts).
	static void reset();

private:
	/// All registered function pointer targets, keyed by AST ID.
	static std::map<int64_t, FuncPtrEntry> s_targets;
	/// Next available function pointer ID.
	static unsigned s_nextId;
};

} // namespace puyasol::builder::eb
