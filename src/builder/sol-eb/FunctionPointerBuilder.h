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
#include <unordered_map>
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
	solidity::frontend::FunctionDefinition const* funcDef; // For visibility check
	std::string subroutineId; // AWST subroutine ID for library/free functions (empty = contract method)
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
	/// Build an expression representing a function reference (pointer).
	/// @param _callerFuncType  Optional: the FunctionType from the calling
	///                         context, which determines Internal vs External.
	///                         When null, derived from _funcDef.
	/// @param _awstName        Optional caller-context awst name (used for
	///                         super references in diamond MRO so multiple
	///                         super.f references for the same target astId
	///                         get distinct dispatcher entries).
	static std::shared_ptr<awst::Expression> buildFunctionReference(
		BuilderContext& _ctx,
		solidity::frontend::FunctionDefinition const* _funcDef,
		awst::SourceLocation const& _loc,
		solidity::frontend::FunctionType const* _callerFuncType = nullptr,
		std::shared_ptr<awst::Expression> _receiverAddress = nullptr,
		std::string const& _awstName = "");

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
	/// Register a function as a potential function pointer target.
	/// @param _awstName  The name used in the AWST (may differ from _funcDef->name()
	///                   for super versions, e.g., "f__super_8").
	static void registerTarget(
		solidity::frontend::FunctionDefinition const* _funcDef,
		solidity::frontend::FunctionType const* _funcType,
		std::string _awstName = "");

	/// Generate the __funcptr_dispatch subroutine(s) for all registered targets.
	/// Groups targets by signature (param types + return types) and generates
	/// one dispatch subroutine per signature group.
	/// Called from ContractBuilder after all methods are translated.
	/// Also populates _outRootSubs with root-level Subroutine copies so that
	/// library subroutines can resolve them via SubroutineID.
	static std::vector<awst::ContractMethod> generateDispatchMethods(
		std::string const& _cref,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Subroutine>>* _outRootSubs = nullptr);

	/// Get the dispatch subroutine name for a given function type signature.
	static std::string dispatchName(
		solidity::frontend::FunctionType const* _funcType);

	/// Set subroutine IDs for registered targets (from m_freeFunctionById map).
	/// Called after all library/free functions are registered.
	static void setSubroutineIds(
		std::unordered_map<int64_t, std::string> const& _idMap);

	/// Set the current contract cref — must be called before translating
	/// function bodies so that library subroutines can construct
	/// SubroutineIDs for dispatch calls.
	static void setCurrentCref(std::string _cref) { s_currentCref = std::move(_cref); }

	/// Clear all registered targets (between contracts).
	static void reset();

private:
	/// Build the internal-dispatch SubroutineCallExpression. Used both by
	/// the standalone internal path and the self-call branch of the external
	/// path. Caller supplies the pointer-id expression (not stored) and the
	/// raw args (coerced to the dispatch parameter types).
	static std::shared_ptr<awst::SubroutineCallExpression> buildDispatchCall(
		BuilderContext& _ctx,
		solidity::frontend::FunctionType const* _funcType,
		std::shared_ptr<awst::Expression> _ptrIdExpr,
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc);

	/// All registered function pointer targets, keyed by (AST ID, awst name).
	/// awst name is empty for default references and `f__super_<callerId>` for
	/// super references — the (id, name) tuple lets diamond MRO produce
	/// distinct dispatcher entries when the same target astId is reached via
	/// different super contexts.
	static std::map<std::pair<int64_t, std::string>, FuncPtrEntry> s_targets;
	/// Next available function pointer ID.
	static unsigned s_nextId;
	/// Dispatch signatures needed (from buildFunctionPointerCall).
	/// Maps dispatch name → FunctionType* for generating empty dispatchers.
	static std::map<std::string, solidity::frontend::FunctionType const*> s_neededDispatches;
	/// Current contract cref — set during contract build so that library
	/// subroutines can construct SubroutineIDs for dispatch calls.
	static std::string s_currentCref;
};

} // namespace puyasol::builder::eb
