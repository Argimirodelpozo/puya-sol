#pragma once

/// @file FunctionSplitter.h
/// Mid-function body splitter — slices a subroutine's body into N pieces
/// along USER-SUPPLIED statement boundaries.
///
/// Pieces are independent Subroutines named `<sub>__piece_<i>_g<groupId>`.
/// Each piece's body is the corresponding slice of the original sub's
/// statements; live variables that cross a split point flow through AVM
/// scratch slot 100 (a single-slot, ARC4-encoded tuple).
///
/// Pieces of the same group are intended to be executed back-to-back — either
/// as `callsub` in-app (if all pieces share a codebox) or as the inner-txn
/// group dance UrosSplitter / orch issues when pieces are in different
/// codeboxes. Each call-frame in the dance writes its piece's live-out
/// vars to slot 100, and the next piece's prologue reads them via
/// `gload <prev_piece_call_txn_index> 100`.
///
/// Scope: this splitter ONLY slices bodies and emits pieces. It does NOT
/// modify the original subroutine's body — leaves it intact for callers
/// to handle however they want (UrosSplitter rewrites main's stub to issue
/// the orch dance; in-app dispatch can simply chain the pieces).

#include "awst/Node.h"
#include "awst/WType.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::splitter
{

class FunctionSplitter
{
public:
	/// AVM scratch slot reserved for live-vars-tuple between pieces.
	/// Slots 0..99 are used by the assembly translator's memory blob and
	/// scratch-based locals; pick something well above that.
	static constexpr int kLiveVarsScratchSlot = 100;

	/// One spec per subroutine to split.
	///   splitPoints: statement indices where splits occur. With N split
	///                points, the body slices into N+1 pieces.
	///                e.g. body has 10 statements, splitPoints = [3, 7]:
	///                  piece_0 = stmts [0..3)
	///                  piece_1 = stmts [3..7)
	///                  piece_2 = stmts [7..10)
	///   groupId: the `_gN` suffix on each piece's name. Pieces with the
	///            same groupId form one logical chain.
	struct PieceSpec
	{
		std::string subroutineName;
		std::vector<size_t> splitPoints;
		int groupId = 0;
	};

	struct SplitResult
	{
		/// New piece subroutines, appended to `_roots` by `splitAt`.
		/// Only populated for Subroutine targets — ContractMethod pieces
		/// are pushed directly onto the parent contract's `methods` list
		/// (counted in `newContractMethodPieces`).
		std::vector<std::shared_ptr<awst::Subroutine>> newSubroutines;

		/// Count of ContractMethod pieces created (already pushed onto
		/// their parent contract's `methods` — not exposed as a list).
		size_t newContractMethodPieces = 0;

		/// Names of subroutines that were split (for downstream tools to
		/// detect "this name now means a chain of pieces").
		std::set<std::string> splitFunctions;

		bool didSplit = false;
	};

	struct VarInfo
	{
		std::string name;
		awst::WType const* wtype = nullptr;
	};

	/// Slice each subroutine in `_specs` and append the resulting pieces
	/// to `_roots`. Original subroutines are left in `_roots` unchanged.
	SplitResult splitAt(
		std::vector<std::shared_ptr<awst::RootNode>>& _roots,
		std::vector<PieceSpec> const& _specs);

private:

	/// Compute variables that are LIVE at a split point: defined in the
	/// statements before the split, AND used in the statements after it.
	/// Sorted by name for deterministic ordering.
	std::vector<VarInfo> computeLiveVars(
		std::vector<std::shared_ptr<awst::Statement>> const& _stmts,
		size_t _splitPoint,
		std::set<std::string> const& _paramNames);

	void collectExprUses(awst::Expression const& _expr,
		std::set<std::string>& _uses);
	void collectStmtUses(awst::Statement const& _stmt,
		std::set<std::string>& _uses);
	void collectStmtDefs(awst::Statement const& _stmt,
		std::set<std::string>& _defs);
	void collectVarType(awst::Expression const& _expr);

	std::map<std::string, awst::WType const*> m_varTypes;
};

} // namespace puyasol::splitter
