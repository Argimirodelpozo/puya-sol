#pragma once

/// @file FunctionSplitter.h
/// Mid-function body splitter — slices a subroutine's body into N pieces
/// at user-supplied statement boundaries.
///
/// Pieces are named `<sub>__piece_<i>_g<groupId>`. Live variables crossing
/// a split point flow through AVM scratch slot 100. Pieces run back-to-back
/// as callsubs (in-program) or via the orch inner-txn dance (cross-chunk),
/// with each piece's epilogue storing live-out vars to slot 100 and the next
/// piece's prologue reading them via `gload <prev_call_txn_index> 100`.
///
/// This splitter only slices bodies and emits pieces; it does not modify the
/// original subroutine (callers handle that — orch dance, in-app chain, etc.).

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
	/// Scratch slot for live-vars tuple between pieces.
	/// Slots 0..99 are used by the memory blob and scratch locals.
	static constexpr int kLiveVarsScratchSlot = 100;

	/// One spec per split target.
	///   splitPoints: statement indices where cuts occur; N points → N+1 pieces.
	///     e.g. 10 stmts, splitPoints=[3,7]: piece_0=[0..3), piece_1=[3..7), piece_2=[7..10)
	///   groupId: `_gN` suffix on piece names; same groupId = one logical chain.
	///   crossChunk: true → pieces run in separate uros chunks via orch's
	///     dispatch_chain; prologue uses `gload <prev_call_txn_idx> 100`.
	///     false → `load 100` (same-txn scratch, in-program callsub or co-located).
	struct PieceSpec
	{
		std::string subroutineName;
		std::vector<size_t> splitPoints;
		int groupId = 0;
		bool crossChunk = false;
		/// Stride between successive piece call txn indices in the inner-txn group.
		/// Default 2 = uros orch convention (install at 2N, call at 2N+1).
		/// Set to 1 for PureHelperExtractor split sidecars (no install txns).
		int prevCallStride = 2;
	};

	struct SplitResult
	{
		/// New piece Subroutines (appended to roots). Empty for ContractMethod
		/// targets — those are pushed directly onto the parent contract's methods.
		std::vector<std::shared_ptr<awst::Subroutine>> newSubroutines;

		/// Count of ContractMethod pieces pushed onto their parent contract.
		size_t newContractMethodPieces = 0;

		/// Names of split subroutines (each name now means a chain of pieces).
		std::set<std::string> splitFunctions;

		bool didSplit = false;
	};

	struct VarInfo
	{
		std::string name;
		awst::WType const* wtype = nullptr;
	};

	/// Slice each subroutine in `_specs`; append pieces to `_roots`.
	/// Original subroutines are left unchanged.
	SplitResult splitAt(
		std::vector<std::shared_ptr<awst::RootNode>>& _roots,
		std::vector<PieceSpec> const& _specs);

private:

	/// Variables live at a split point: defined before ∩ used after.
	/// Sorted by name for determinism.
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
