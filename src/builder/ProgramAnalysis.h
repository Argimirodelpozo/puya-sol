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

	// Memo used by ParamMutationDetector. It belongs to this program rather
	// than to the process; mutable because detection is demand-driven.
	mutable std::map<int64_t, std::set<int64_t>> transitivelyMutatedParams;
	mutable std::set<int64_t> mutationAnalysisInProgress;

	static ProgramAnalysis analyze(
		solidity::frontend::CompilerStack& _compiler,
		bool _evmStorageLayout);
};

} // namespace puyasol::builder
