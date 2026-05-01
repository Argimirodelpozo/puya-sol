#pragma once

#include "awst/Node.h"
#include "builder/ContractBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/interface/CompilerStack.h>

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

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

	// ── Build phases (executed in order from build()) ──

	/// Phase 1: walk every source unit and register every library function and
	/// every file-level free function in `m_libraryFunctionIds` and
	/// `m_freeFunctionById`. Disambiguates overloaded names by parameter count.
	void registerFunctionIds(solidity::frontend::CompilerStack& _compiler, std::string const& _sourceFile);

	/// Phase 1.5: pre-set the function-pointer dispatch cref to the first
	/// deployable contract — library subroutines need this to construct
	/// SubroutineIDs for cross-call dispatch tables.
	void presetDispatchCref(solidity::frontend::CompilerStack& _compiler, std::string const& _sourceFile);

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
};

} // namespace puyasol::builder
