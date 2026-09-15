#pragma once

#include <map>

#include "awst/Node.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/context/CompilationSession.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/types/TypeMapper.h"
#include "builder/solc/StorageRefPointer.h" // containsMappingType + storageRefPointerReturn

#include <libsolidity/ast/AST.h>
#include <libsolidity/interface/CompilerStack.h>
#include <liblangutil/EVMVersion.h>

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

// containsMappingType lives in builder/solc/StorageRefPointer.h so
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
		std::map<std::string, std::string> const& _sourceAliases = {},
		TargetProfile _targetProfile = {}
	);

private:
	CompilationSession m_session;
	std::unique_ptr<StorageMapper> m_storageMapper;

	/// Free/library functions whose lowering needs a concrete host contract:
	/// modifier chains, function-pointer dispatch, or default-layout inline
	/// storage assembly. Their reverse caller closure is hosted as well because
	/// a root Subroutine cannot call a contract instance method.
	std::vector<solidity::frontend::FunctionDefinition const*> m_hostBoundFunctions;
	std::set<int64_t> m_hostBoundFunctionIds;
	/// solc's filesystemFriendlyName rule for colliding contract names.
	std::map<std::string, std::string> m_artifactNames;
	std::vector<solidity::frontend::ContractDefinition const*> m_selectorContracts;

	// ── Build phases (executed in order from build()) ──
	void collectHostBoundFunctions();

	/// Free/library bodies share eligibility, symbols and host-bound decisions.
	void translateFreestandingFunctions(
		std::string const& _sourceFile,
		std::vector<std::shared_ptr<awst::RootNode>>& _roots);

	/// Phase 4: translate concrete contracts via ContractBuilder; DCE method
	/// bodies; synthesize a `__dummy` ARC4 method for constructor-only contracts.
	void translateContracts(
		std::string const& _sourceFile,
		uint64_t _opupBudget,
		std::map<std::string, uint64_t> const& _ensureBudget,
		std::vector<std::shared_ptr<awst::RootNode>>& _roots);

	// ── translateContracts phases ───────────────────────────────────────
	/// Whether any reachable root needs the shape-keyed EVM storage runtime.
	bool prescanEvmStorageLayout();

	/// One ContractBuilder run over `_contract`; its dispatch subroutines go
	/// straight to `_roots`. The unit-global EVM storage runtime is emitted
	/// with the first contract when needed (`_emittedEvmStorageRuntime` latches).
	std::shared_ptr<awst::Contract> translateContract(
		solidity::frontend::ContractDefinition const& _contract,
		std::string const& _sourceFile,
		uint64_t _opupBudget,
		std::map<std::string, uint64_t> const& _ensureBudget,
		bool _evmStorageRuntimeNeeded,
		bool& _emittedEvmStorageRuntime,
		std::vector<std::shared_ptr<awst::RootNode>>& _roots);

	/// Build an awst::Subroutine for a library function or free function.
	/// `_libraryName` is empty for free functions; forwarded to ContractContext
	/// as `contractName` for member-name resolution.
	std::shared_ptr<awst::Subroutine> buildFreestandingSubroutine(
		solidity::frontend::FunctionDefinition const& _func,
		std::string const& _sourceFile,
		std::string const& _qualifiedName,
		std::string const& _subroutineId,
		std::string const& _libraryName,
		BuildArtifacts::PathSpecialization const* _pathSpec = nullptr);

	// ── buildFreestandingSubroutine phases ──────────────────────────────
	void buildFreestandingParams(
		solidity::frontend::FunctionDefinition const& function,
		std::string const& sourceFile, awst::Subroutine& sub);
	void prependFreestandingReturnInits(
		solidity::frontend::FunctionDefinition const& _func,
		awst::Subroutine& sub,
		awst::SourceLocation const& loc);
	void synthesizeFreestandingImplicitReturn(
		solidity::frontend::FunctionDefinition const& _func,
		awst::Subroutine& sub,
		sol_ast::FunctionContext& fnCtx,
		awst::SourceLocation const& loc);
};

} // namespace puyasol::builder
