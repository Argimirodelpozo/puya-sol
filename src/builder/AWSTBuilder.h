#pragma once

#include "awst/Node.h"
#include "builder/CompilationSession.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-ast/StorageRefPointer.h" // containsMappingType + storageRefPointerReturn

#include <libsolidity/ast/AST.h>
#include <libsolidity/interface/CompilerStack.h>
#include <liblangutil/EVMVersion.h>

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

// containsMappingType lives in builder/sol-ast/StorageRefPointer.h so
// storageRefPointerReturn can share it; visible here for AWSTBuilder.cpp,
// SolInternalCall.cpp, FunctionBuilder.cpp, PublicGetterBuilder.cpp.

/// Top-level builder that drives the Solidity AST → AWST transformation.
/// Uses CompilerStack for parsing and type-checking, then visits all contracts.
class AWSTBuilder
{
public:
	BuildArtifacts const& artifacts() const { return m_session.artifacts; }

	/// Build AWST from a Solidity source file.
	/// Returns root nodes (contracts, subroutines) for JSON serialization.
	/// _opupBudget: if > 0, inject ensure_budget(_opupBudget) into public methods.
	std::vector<std::shared_ptr<awst::RootNode>> build(
		solidity::frontend::CompilerStack& _compiler,
		std::string const& _sourceFile,
		uint64_t _opupBudget = 0,
		std::map<std::string, uint64_t> const& _ensureBudget = {},
		bool _viaYulBehavior = false,
		std::map<std::string, std::string> const& _sourceAliases = {},
		TargetProfile _targetProfile = {}
	);

private:
	CompilationSession m_session;
	std::unique_ptr<StorageMapper> m_storageMapper;

	/// Canonical solc declaration ID → opaque AWST function identity.
	FunctionSymbolTable m_functionSymbols;

	/// Library functions with function-pointer parameters that must be inlined
	/// into each using-contract rather than emitted as root Subroutines.
	/// The fn-ptr dispatcher may invoke contract instance methods, which puya
	/// rejects from a root Subroutine scope.
	/// Free/library functions whose lowering needs a concrete host contract
	/// (function-pointer dispatch or default-layout inline storage assembly).
	std::vector<solidity::frontend::FunctionDefinition const*> m_hostBoundFunctions;
	std::vector<solidity::frontend::ContractDefinition const*> m_selectorContracts;

	// ── Build phases (executed in order from build()) ──
	// Phase 1: registerFunctionIds → m_functionSymbols.
	// Phase 1.5: presetDispatchCref → fn-ptr dispatch cref (first deployable contract).
	// Both defined in builder/FunctionIdRegistry.h.

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

	/// Phase 4: translate concrete contracts via ContractBuilder; DCE method
	/// bodies; synthesize a `__dummy` ARC4 method for constructor-only contracts.
	void translateContracts(
		solidity::frontend::CompilerStack& _compiler,
		std::string const& _sourceFile,
		uint64_t _opupBudget,
		std::map<std::string, uint64_t> const& _ensureBudget,
		bool _viaYulBehavior,
		std::vector<std::shared_ptr<awst::RootNode>>& _roots);

	/// Build an awst::Subroutine for a library function or free function.
	/// `_libraryName` is empty for free functions; forwarded to ContractContext
	/// as `contractName` for member-name resolution.
	std::shared_ptr<awst::Subroutine> buildFreestandingSubroutine(
		solidity::frontend::FunctionDefinition const& _func,
		std::string const& _sourceFile,
		std::string const& _qualifiedName,
		std::string const& _subroutineId,
		std::string const& _libraryName);

	// ── buildFreestandingSubroutine phases ──────────────────────────────
	void buildFreestandingParams(
		solidity::frontend::FunctionDefinition const& _func,
		std::string const& _sourceFile,
		awst::Subroutine& sub,
		std::set<size_t> const& slotParams,
		std::set<size_t>& mappingStorageParams,
		std::set<size_t>& blobAggParams,
		std::set<size_t>& evmSlotRefParams);
	void collectFreestandingAugmentedParams(
		solidity::frontend::FunctionDefinition const& _func,
		std::set<size_t> const& mappingStorageParams,
		std::set<size_t> const& blobAggParams,
		std::vector<size_t>& storageParamIndices,
		std::vector<size_t>& memoryRefParamIndices);
	void computeFreestandingReturnType(
		solidity::frontend::FunctionDefinition const& _func,
		awst::Subroutine& sub,
		std::vector<size_t> const& storageParamIndices,
		std::vector<size_t> const& memoryRefParamIndices);
	void registerFreestandingParamContext(
		solidity::frontend::FunctionDefinition const& _func,
		sol_ast::FunctionContext& fnCtx,
		awst::Subroutine const& sub,
		std::set<size_t> const& slotParams,
		std::set<size_t> const& mappingStorageParams,
		std::set<size_t> const& blobAggParams,
		std::set<size_t> const& evmSlotRefParams);
	void registerFreestandingReturnParams(
		solidity::frontend::FunctionDefinition const& _func,
		sol_ast::FunctionContext& fnCtx,
		std::set<size_t> const& blobAggParams);
	void prependFreestandingReturnInits(
		solidity::frontend::FunctionDefinition const& _func,
		awst::Subroutine& sub,
		awst::SourceLocation const& loc);
	void synthesizeFreestandingImplicitReturn(
		solidity::frontend::FunctionDefinition const& _func,
		awst::Subroutine& sub,
		sol_ast::FunctionContext& fnCtx,
		std::vector<size_t> const& storageParamIndices,
		std::vector<size_t> const& memoryRefParamIndices,
		awst::SourceLocation const& loc);
};

} // namespace puyasol::builder
