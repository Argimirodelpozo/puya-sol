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

/// Registry of internal function pointer targets; used to generate dispatch tables.
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
	/// Internal → uint64, External → bytes[12].
	static awst::WType const* mapFunctionType(
		solidity::frontend::FunctionType const* _funcType);

	/// Build a function reference expression (internal: IntegerConstant id;
	/// external: bytes[12] = itob(appId) ++ selector).
	/// @param _callerFuncType  Determines Internal vs External when both exist
	///                         (e.g. `this.g` is External). Derived from _funcDef if null.
	/// @param _awstName        For super refs in diamond MRO: distinct entries
	///                         per caller context for the same target astId.
	static std::shared_ptr<awst::Expression> buildFunctionReference(
		ContractContext& _ctx,
		solidity::frontend::FunctionDefinition const* _funcDef,
		awst::SourceLocation const& _loc,
		solidity::frontend::FunctionType const* _callerFuncType = nullptr,
		std::shared_ptr<awst::Expression> _receiverAddress = nullptr,
		std::string const& _awstName = "");

	/// Call through a function pointer (internal: dispatch table; external: inner app call).
	static std::shared_ptr<awst::Expression> buildFunctionPointerCall(
		ContractContext& _ctx,
		std::shared_ptr<awst::Expression> _ptrExpr,
		solidity::frontend::FunctionType const* _funcType,
		std::vector<std::shared_ptr<awst::Expression>> _args,
		awst::SourceLocation const& _loc);

	/// Register a function pointer target.
	/// @param _awstName  AWST name (may differ from _funcDef->name() for super refs, e.g. "f__super_8").
	static void registerTarget(
		solidity::frontend::FunctionDefinition const* _funcDef,
		solidity::frontend::FunctionType const* _funcType,
		std::string _awstName = "");

	/// Generate __funcptr_dispatch subroutines (one per signature group).
	/// Called after all methods are translated. Populates _outRootSubs so
	/// library subroutines can resolve dispatch via SubroutineID.
	static std::vector<awst::ContractMethod> generateDispatchMethods(
		ContractContext& _ctx,
		std::string const& _cref,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Subroutine>>* _outRootSubs = nullptr);

	/// Dispatch subroutine name for a given function type signature.
	static std::string dispatchName(
		solidity::frontend::FunctionType const* _funcType);

	/// Set subroutine IDs for registered targets; call after all library/free fns are registered.
	static void setSubroutineIds(
		std::unordered_map<int64_t, std::string> const& _idMap);

	/// Set current contract cref before translating function bodies
	/// (library subroutines need it to construct SubroutineIDs).
	static void setCurrentCref(std::string _cref) { s_currentCref = std::move(_cref); }

	/// Clear all registered targets between contracts.
	static void reset();

private:
	/// Build internal-dispatch SubroutineCallExpression (shared by internal
	/// and external self-call paths). Args are coerced to dispatch param types.
	static std::shared_ptr<awst::SubroutineCallExpression> buildDispatchCall(
		ContractContext& _ctx,
		solidity::frontend::FunctionType const* _funcType,
		std::shared_ptr<awst::Expression> _ptrIdExpr,
		std::vector<std::shared_ptr<awst::Expression>> const& _args,
		awst::SourceLocation const& _loc);

	// Keyed by (AST ID, awst name); empty name for default refs, "f__super_<id>"
	// for super refs — lets diamond MRO produce distinct entries per caller context.
	static std::map<std::pair<int64_t, std::string>, FuncPtrEntry> s_targets;
	static unsigned s_nextId; ///< 0 = invalid/zero-initialized.
	// dispatch name → FunctionType* for signatures needed (from buildFunctionPointerCall).
	static std::map<std::string, solidity::frontend::FunctionType const*> s_neededDispatches;
	static std::string s_currentCref;
};

} // namespace puyasol::builder::eb
