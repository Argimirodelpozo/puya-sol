#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

namespace solidity::frontend
{
class CompilerStack;
class ContractDefinition;
class FunctionCall;
class FunctionDefinition;
}

namespace puyasol::builder
{

/// Source-level reference effects for one callable body in one concrete
/// contract context. Parameter POSITIONS are stable across virtual overrides;
/// their declaration IDs are not.
struct ParameterMutationSummary
{
	std::set<size_t> mutatedParameterIndices;

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
	std::set<int64_t> boxKeyedStructs;
	std::set<int64_t> refPassedStructs;
	std::set<int64_t> reassignedMemoryLocals;
	std::set<int64_t> structRefOffsetParams;
	std::set<int64_t> callablesWithInlineAssembly;
	/// Callables whose parsed Yul contains sload/sstore or exposes a `.slot`
	/// handle that can be dereferenced by later Solidity expressions.
	std::set<int64_t> callablesWithStorageAssembly;
	/// Declaration AST ids referenced through a Yul `.slot` external reference.
	/// Centralizing this avoids rescanning individual function bodies when
	/// planning storage-reference parameter representations.
	std::set<int64_t> asmSlotReferenceDeclarations;
	/// Contract AST id → function ids reached through an internal call edge.
	/// Entry-only public methods are absent; internal-dispatch targets are present.
	std::map<int64_t, std::set<int64_t>> internallyCalledFunctions;
	std::set<int64_t> contractsWithReachabilityGraphs;
	std::map<int64_t, std::set<int64_t>> reachableFunctionsByContract;
	bool hasReachabilityGraphs = false;
	std::set<int64_t> reachableFunctionIds;
	std::set<int64_t> reachableCallableIds;

	bool isCalledInternally(int64_t _contractId, int64_t _functionId) const
	{
		auto const contract = internallyCalledFunctions.find(_contractId);
		return contract != internallyCalledFunctions.end()
			&& contract->second.count(_functionId) != 0;
	}

	bool hasContractReachability(int64_t _contractId) const
	{
		return contractsWithReachabilityGraphs.count(_contractId) != 0;
	}

	bool isFunctionReachable(int64_t _contractId, int64_t _functionId) const
	{
		auto const contract = reachableFunctionsByContract.find(_contractId);
		return contract != reachableFunctionsByContract.end()
			&& contract->second.count(_functionId) != 0;
	}

	/// Function declarations indexed by globally unique solc ID. Mutation
	/// analysis uses the caller to give `super.f()` its lexical search start.
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

	/// Effects of the concrete function reached by a reference-preserving
	/// internal `_call`. Returns null for indirect and ABI-boundary calls.
	ParameterMutationSummary const* parameterMutationsForCall(
		solidity::frontend::ContractDefinition const* _mostDerived,
		int64_t _callerCallableId,
		solidity::frontend::FunctionCall const& _call) const;

	static ProgramAnalysis analyze(
		solidity::frontend::CompilerStack& _compiler,
		bool _evmStorageLayout);
};

} // namespace puyasol::builder
