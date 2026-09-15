#pragma once

#include "builder/solc/ProxyFacts.h"

#include <libyul/SideEffects.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace solidity::frontend
{
class CompilerStack;
class ContractDefinition;
class Expression;
class FunctionCall;
class FunctionDefinition;
class IndexAccess;
}

namespace puyasol::builder
{

struct PreparedAssembly;

/// Reachable creation effects, before selecting an AVM storage backend.
/// The callable set is creation-only, plus explicit target-adaptation edges;
/// it is never closed over the union of deployed source references.
struct CreationEffects
{
	std::set<int64_t> reachableCallables;
	std::set<int64_t> stateReferences;
	bool createsContract = false;
	bool externalCall = false;
	bool messageContext = false;
	bool nativeContext = false;
	bool assembly = false;
};

/// Source provenance for a single storage reference return. AVM carrier types
/// are selected by TypeMapper; these facts are independent of the call site.
struct StorageReferenceReturnFacts
{
	struct PointerAlias
	{
		size_t parameter;
		std::string field;
	};
	solidity::frontend::IndexAccess const* indexedReturn = nullptr;
	std::optional<PointerAlias> pointerAlias;
	bool bytesKeyed = false;
	bool slotHandle = false;
};

/// Source-level reference effects for one callable body in one concrete
/// contract context. Parameter POSITIONS are stable across virtual overrides;
/// their declaration IDs are not.
struct ParameterMutationSummary
{
	std::set<size_t> mutatedParameterIndices;
	/// Actual solc/Yul effects, closed over host-resolved Solidity call edges.
	/// Separate from referent mutation: memory reads and non-removable control
	/// flow are not writes, and local scratch writes need not mutate a parameter.
	solidity::yul::SideEffects assemblyEffects;

	bool mutates(size_t _index) const
	{
		return mutatedParameterIndices.count(_index) != 0;
	}
};

/// Immutable whole-program facts computed once before AWST translation.
/// Keeping these in a build-owned value avoids reset-order dependencies and
/// lets multiple compiler sessions coexist safely.
struct ProgramAnalysis
{
	proxies::ProxyFacts proxy;
	/// Source-unit order, including abstract contracts, libraries and interfaces.
	std::vector<solidity::frontend::ContractDefinition const*> contracts;
	/// Validated libs/AVM.sol function declaration id -> native library.
	std::map<int64_t, std::string> avmIntrinsics;
	std::map<int64_t, CreationEffects> creationEffects;
	std::set<int64_t> boxKeyedStructs;
	std::set<int64_t> refPassedStructs;
	std::set<int64_t> reassignedMemoryLocals;
	/// Conservative reference-assignment edges (destination -> possible sources),
	/// using solc declaration IDs and data locations. Not a runtime points-to map.
	std::map<int64_t, std::set<int64_t>> referenceAssignments;
	/// Aliases connected to a rebound memory name need the existing pointer
	/// representation locally; stable aliases keep their value/write-back path.
	std::set<int64_t> memoryIdentityDeclarations;
	/// Single-declaration initializer ASTs, indexed once by solc declaration ID.
	/// Source provenance only: live reference bindings still win after reassignments.
	std::map<int64_t, solidity::frontend::Expression const*> localInitializers;
	/// Direct function initializers of locals that solc never marks as written
	/// and that no assembly block references. Safe specialization, not a
	/// translation-order-dependent cache of a variable's current value.
	std::map<int64_t, solidity::frontend::Expression const*> stableFunctionPointers;
	std::set<int64_t> structRefOffsetParams;
	std::set<int64_t> callablesWithInlineAssembly;
	std::map<int64_t, std::shared_ptr<PreparedAssembly const>> preparedAssemblies;
	/// Storage declarations carrying runtime logical slots: assigned via Yul
	/// or receiving slot-return components through the solc transfer graph.
	std::set<int64_t> slotHandleDeclarations;
	std::map<int64_t, StorageReferenceReturnFacts> storageReferenceReturns;
	StorageReferenceReturnFacts const& storageReturnFacts(
		solidity::frontend::FunctionDefinition const* _function) const;
	/// Callables using logical storage slots, through Yul or storage-reference
	/// return transport. Named-storage hosts need their word dispatcher too.
	std::set<int64_t> callablesWithStorageSlotAccess;
	/// IndexAccess AST ids that ARE a storage-ref pointer function's return
	/// (`function g(uint i) internal returns (R storage) { return m[i]; }`).
	/// In that position `m[i]` names a LOCATION, not a value: FunctionBuilder
	/// rewrites the return to the bare uint64 index and the call site
	/// reconstitutes the access. Any lowering that would materialise the
	/// element instead — multi-box paging, for one — must stand down here.
	std::set<int64_t> storageRefPointerReturnAccesses;
	/// Declaration AST ids referenced through a Yul `.slot` external reference.
	/// Centralizing this avoids rescanning individual function bodies when
	/// planning storage-reference parameter representations.
	std::set<int64_t> asmSlotReferenceDeclarations;
	/// Contract AST id → function ids reached through an internal call edge.
	/// Entry-only public methods are absent; internal-dispatch targets are present.
	std::map<int64_t, std::set<int64_t>> internallyCalledFunctions;
	std::map<int64_t, std::set<int64_t>> reachableCallablesByContract;
	bool hasReachabilityGraphs = false;
	std::set<int64_t> reachableCallableIds;
	/// Cached from solc's library graphs and resolved declaration references. Complements the
	/// context-specific creation/deployed graphs, which can stop at library
	/// entrypoints. Includes modifier bodies and function-pointer references.
	std::map<int64_t, std::set<int64_t>> callableReferences;
	std::map<int64_t, std::set<int64_t>> callableCallers;

	/// Close a seed set over the cached source-reference edges, without AST walks.
	void closeCallableReferences(std::set<int64_t>& _ids) const;

	bool isCalledInternally(int64_t _contractId, int64_t _functionId) const
	{
		auto const contract = internallyCalledFunctions.find(_contractId);
		return contract != internallyCalledFunctions.end()
			&& contract->second.count(_functionId) != 0;
	}

	bool hasContractReachability(int64_t _contractId) const
	{
		return reachableCallablesByContract.contains(_contractId);
	}

	bool isCallableReachable(int64_t _contractId, int64_t _functionId) const
	{
		auto const contract = reachableCallablesByContract.find(_contractId);
		return contract != reachableCallablesByContract.end()
			&& contract->second.count(_functionId) != 0;
	}

	/// Function declarations indexed by globally unique solc ID.
	std::map<int64_t, solidity::frontend::FunctionDefinition const*>
		functionDeclarations;

	/// `(mostDerivedContractId, exactFunctionBodyId)` → fixed-point summary.
	/// Mutable because summaries are demand-computed from the roots that the
	/// selected contract actually lowers; completed values are immutable.
	mutable std::map<std::pair<int64_t, int64_t>, ParameterMutationSummary>
		parameterMutationSummaries;

	/// Effects of one EXACT implementation body. `_mostDerived` controls
	/// virtual calls made by that body; `_function` itself is not re-resolved.
	ParameterMutationSummary const& parameterMutations(
		solidity::frontend::ContractDefinition const* _mostDerived,
		solidity::frontend::FunctionDefinition const& _function) const;

	static ProgramAnalysis analyze(
		solidity::frontend::CompilerStack& _compiler,
		bool _evmStorageLayout);
};

} // namespace puyasol::builder
