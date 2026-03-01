#pragma once

#include "awst/Node.h"
#include "splitter/SizeEstimator.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::splitter
{

/// Analyzes the call graph of a contract's methods and subroutines,
/// then produces a partition recommendation for splitting oversized contracts.
class CallGraphAnalyzer
{
public:
	struct FunctionInfo
	{
		std::string name;
		size_t estimatedSize = 0;
		bool hasStateAccess = false;      // uses box/global/local storage
		std::set<std::string> calls;      // functions this calls
		std::set<std::string> calledBy;   // functions that call this
		bool isContractMethod = false;    // is an ABI/bare method (not just a subroutine)
		bool isARC4Method = false;        // has ARC4 method config (externally callable)
	};

	struct SplitRecommendation
	{
		/// Each partition is a set of function names that should go together.
		/// Partition 0 is always the orchestrator (original contract methods).
		std::vector<std::vector<std::string>> partitions;

		/// Shared utility functions that must be duplicated to each partition that uses them.
		std::vector<std::string> sharedUtilities;

		/// Estimated total cross-partition data in bytes.
		size_t estimatedCrossPartitionDataBytes = 0;

		/// Whether splitting is recommended.
		bool shouldSplit = false;
	};

	/// Analyze a contract's call graph and produce split recommendations.
	/// @param _rewrittenFunctions Functions that were split by FunctionSplitter
	///        (their parent stubs go to the orchestrator, not helpers).
	/// @param _mutableSharedFunctions Functions whose chunks modify ReferenceArray
	///        params (chunks of these must stay in the same partition).
	SplitRecommendation analyze(
		awst::Contract const& _contract,
		std::vector<std::shared_ptr<awst::RootNode>> const& _subroutines,
		SizeEstimator::Estimate const& _sizes,
		std::set<std::string> const& _rewrittenFunctions = {},
		std::set<std::string> const& _mutableSharedFunctions = {}
	);

private:
	/// Build the call graph from a contract and its subroutines.
	void buildCallGraph(
		awst::Contract const& _contract,
		std::vector<std::shared_ptr<awst::RootNode>> const& _subroutines
	);

	/// Scan an expression for subroutine calls and state access.
	void scanExpression(awst::Expression const& _expr, std::string const& _caller);

	/// Scan a statement for subroutine calls and state access.
	void scanStatement(awst::Statement const& _stmt, std::string const& _caller);

	/// Scan a block for subroutine calls and state access.
	void scanBlock(awst::Block const& _block, std::string const& _caller);

	/// Compute transitive closure of call dependencies.
	std::set<std::string> transitiveDeps(std::string const& _func) const;

	/// Greedy bin-packing of functions into partitions.
	std::vector<std::vector<std::string>> binPack(
		SizeEstimator::Estimate const& _sizes,
		size_t _maxSize,
		std::set<std::string> const& _rewrittenFunctions = {},
		std::set<std::string> const& _mutableSharedFunctions = {}
	) const;

	/// Function info map: name → info
	std::map<std::string, FunctionInfo> m_functions;

	/// All subroutine IDs seen (for resolving SubroutineID targets)
	std::map<std::string, std::string> m_subroutineIdToName;
};

} // namespace puyasol::splitter
