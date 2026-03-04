#pragma once

#include "awst/Node.h"
#include "splitter/CallGraphAnalyzer.h"
#include "splitter/SizeEstimator.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::splitter
{

/// Splits an oversized contract into an orchestrator + helper contracts.
/// Each contract gets its own AWST root list (separate awst.json for puya).
/// Helpers wrap assigned subroutines as ABI methods.
class ContractSplitter
{
public:
	/// One contract's AWST: its Contract node + its subroutines.
	struct ContractAWST
	{
		std::string contractId;
		std::string contractName;
		std::vector<std::shared_ptr<awst::RootNode>> roots;
	};

	struct SplitResult
	{
		/// Per-contract AWST root lists. Index 0..N-1 are helpers, last is orchestrator.
		std::vector<ContractAWST> contracts;

		/// Whether splitting actually occurred.
		bool didSplit = false;
	};

	/// Split the contract according to the recommendation.
	SplitResult split(
		std::shared_ptr<awst::Contract> _original,
		std::vector<std::shared_ptr<awst::RootNode>>& _roots,
		CallGraphAnalyzer::SplitRecommendation const& _recommendation
	);

private:
	/// Build a subroutine name→root map from roots.
	std::map<std::string, std::shared_ptr<awst::Subroutine>> buildSubroutineMap(
		std::vector<std::shared_ptr<awst::RootNode>> const& _roots
	);

	/// Collect transitive subroutine dependencies for a set of function names.
	/// Walks subroutine bodies to find SubroutineCallExpression targets.
	/// Skips functions in _otherPartitionFuncs (they'll be cross-contract calls).
	std::set<std::string> collectDependencies(
		std::set<std::string> const& _funcNames,
		std::map<std::string, std::shared_ptr<awst::Subroutine>> const& _subMap,
		std::set<std::string> const& _otherPartitionFuncs = {}
	);

	/// Scan an expression for subroutine ID references.
	void scanExprForSubroutineIds(
		awst::Expression const& _expr,
		std::set<std::string>& _ids,
		std::map<std::string, std::string> const& _idToName
	);

	/// Scan a statement for subroutine ID references.
	void scanStmtForSubroutineIds(
		awst::Statement const& _stmt,
		std::set<std::string>& _ids,
		std::map<std::string, std::string> const& _idToName
	);

	/// Create a helper Contract node that wraps assigned subroutines as ABI methods.
	std::shared_ptr<awst::Contract> createHelperContract(
		awst::Contract const& _original,
		int _helperIndex,
		std::vector<std::string> const& _functionNames,
		std::map<std::string, std::shared_ptr<awst::Subroutine>> const& _subMap
	);

	/// Build a thin orchestrator Contract that exposes the original ABI interface
	/// but with stub method bodies (return default values). No subroutines needed.
	std::shared_ptr<awst::Contract> buildThinOrchestrator(
		awst::Contract const& _original
	);

	/// Build a stub method body that returns a default value for the given return type.
	std::shared_ptr<awst::Block> buildStubBody(
		awst::SourceLocation const& _loc,
		awst::WType const* _returnType
	);

	/// Scan a subroutine for raw SubroutineID targets (collecting IDs directly).
	void scanSubroutineForRawIds(
		awst::Subroutine const& _sub,
		std::set<std::string>& _ids
	);

	/// Scan an expression for raw SubroutineID targets.
	void scanExprForRawIds(
		awst::Expression const& _expr,
		std::set<std::string>& _ids
	);

	/// Scan a statement for raw SubroutineID targets.
	void scanStmtForRawIds(
		awst::Statement const& _stmt,
		std::set<std::string>& _ids
	);

	/// Build a clear program that always approves.
	awst::ContractMethod buildClearProgram(
		awst::SourceLocation const& _loc,
		std::string const& _cref
	);
};

} // namespace puyasol::splitter
