#pragma once

#include "awst/Node.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-ast/StorageRefPointer.h" // containsMappingType + storageRefPointerReturn

#include <libsolidity/ast/AST.h>
#include <libsolidity/interface/CompilerStack.h>

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

// containsMappingType lives in builder/sol-ast/StorageRefPointer.h (included
// above) so storageRefPointerReturn can share it; it remains visible in this
// namespace for the existing call sites (AWSTBuilder.cpp, SolInternalCall.cpp,
// FunctionBuilder.cpp, PublicGetterBuilder.cpp).

/// Top-level builder that drives the Solidity AST → AWST transformation.
/// Uses CompilerStack for parsing and type-checking, then visits all contracts.
class AWSTBuilder
{
public:
	/// Build AWST from a Solidity source file.
	/// Returns root nodes (contracts, subroutines) for JSON serialization.
	/// _opupBudget: if > 0, inject ensure_budget(_opupBudget) into public methods.
	std::vector<std::shared_ptr<awst::RootNode>> build(
		solidity::frontend::CompilerStack& _compiler,
		std::string const& _sourceFile,
		uint64_t _opupBudget = 0,
		std::map<std::string, uint64_t> const& _ensureBudget = {},
		bool _viaYulBehavior = false
	);

private:
	TypeMapper m_typeMapper;
	std::unique_ptr<StorageMapper> m_storageMapper;

	/// Registry of library function IDs, populated during the library translation pass.
	LibraryFunctionIdMap m_libraryFunctionIds;

	/// Maps free function AST ID → subroutine ID, for operator overload resolution.
	std::unordered_map<int64_t, std::string> m_freeFunctionById;

	/// Library functions with internal function-pointer parameters. Marked
	/// "internalizable" because they need to be promoted into INTERNAL
	/// METHODS of each using-contract (not emitted as root Subroutines).
	/// Their body invokes the funcptr dispatcher whose case branches may
	/// invoke contract instance methods — puya refuses to call instance
	/// methods from a root Subroutine scope, so we host the library function
	/// inside the contract instead.
	std::vector<solidity::frontend::FunctionDefinition const*> m_internalizableLibFuncs;

	// ── Build phases (executed in order from build()) ──
	// Phase 1: `registerFunctionIds` walks every source unit's library +
	// file-level functions into `m_libraryFunctionIds` and `m_freeFunctionById`.
	// Phase 1.5: `presetDispatchCref` pre-sets the function-pointer dispatch
	// cref to the first deployable contract. Both live in
	// `builder/FunctionIdRegistry.h`.

	/// Phase 2: translate library functions into Subroutine root nodes.
	void translateLibraryFunctions(
		solidity::frontend::CompilerStack& _compiler,
		std::string const& _sourceFile,
		std::vector<std::shared_ptr<awst::RootNode>>& _roots);

	/// Phase 3: translate file-level free functions into Subroutine root nodes.
	void translateFreeFunctions(
		solidity::frontend::CompilerStack& _compiler,
		std::string const& _sourceFile,
		std::vector<std::shared_ptr<awst::RootNode>>& _roots);

	/// Phase 4: translate concrete (non-interface, non-abstract, non-library)
	/// contracts via ContractBuilder. Drops dead code in each method body and
	/// synthesizes a no-op `__dummy` ARC4 method for constructor-only contracts.
	void translateContracts(
		solidity::frontend::CompilerStack& _compiler,
		std::string const& _sourceFile,
		uint64_t _opupBudget,
		std::map<std::string, uint64_t> const& _ensureBudget,
		bool _viaYulBehavior,
		std::vector<std::shared_ptr<awst::RootNode>>& _roots);

	/// Build an awst::Subroutine root node from either a library function or a
	/// file-level free function. The two phases share virtually identical logic;
	/// `_libraryName` is the library this function came from (empty for free
	/// functions) and is forwarded to the ContractContext as the `contractName`
	/// for member-name resolution.
	std::shared_ptr<awst::Subroutine> buildFreestandingSubroutine(
		solidity::frontend::FunctionDefinition const& _func,
		std::string const& _sourceFile,
		std::string const& _qualifiedName,
		std::string const& _subroutineId,
		std::string const& _libraryName);
};

} // namespace puyasol::builder
