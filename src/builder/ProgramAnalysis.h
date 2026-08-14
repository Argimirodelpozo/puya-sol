#pragma once

#include <cstdint>
#include <map>
#include <set>

namespace solidity::frontend
{
class CompilerStack;
}

namespace puyasol::builder
{

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

	// Memo used by ParamMutationDetector. It belongs to this program rather
	// than to the process; mutable because detection is demand-driven.
	mutable std::map<int64_t, std::set<int64_t>> transitivelyMutatedParams;
	mutable std::set<int64_t> mutationAnalysisInProgress;

	static ProgramAnalysis analyze(
		solidity::frontend::CompilerStack& _compiler,
		bool _evmStorageLayout);
};

} // namespace puyasol::builder
