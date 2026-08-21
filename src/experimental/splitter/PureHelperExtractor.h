#pragma once

/// @file PureHelperExtractor.h
/// Extract pure Subroutines into standalone helper Contracts called via
/// inner-txn ApplicationCall.
///
/// Large pure helpers (math-heavy, no state) bloat every chunk that reaches
/// them. Lifting each into its own one-method Contract removes its bytecode
/// from the primary program (puya DCE drops the rewritten site). Cost: one
/// inner-txn per call. Return via ABI `log` convention; no state sharing.
///
/// Pipeline: AFTER --fn-split, BEFORE --uros-splitter.

#include "awst/Node.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::splitter
{

class PureHelperExtractor
{
public:
	struct ExtractedHelper
	{
		/// Original Subroutine ID (no longer reachable from non-helper roots).
		std::string subId;

		/// Synthesized helper Contract ID (hosts one ABI-routed approval).
		std::string helperContractId;

		/// TMPL_ var baked into rewritten call sites; harness substitutes
		/// the helper's app id at deploy time (like TMPL_UROS_ORCH_APP_ID).
		std::string templateVarName;

		/// 4-byte selector (sha512_256(canonicalSig)[:4]) used by both
		/// rewritten call sites (ApplicationArgs[0]) and the helper's router.
		std::vector<uint8_t> selector;
	};

	struct Result
	{
		std::vector<std::shared_ptr<awst::Contract>> helperContracts;
		std::vector<ExtractedHelper> extracted;
		bool didExtract = false;
	};

	/// Per-helper body split for subs that exceed the 16 KiB AVM program cap.
	/// PureHelperExtractor emits one sidecar per piece and rewrites call
	/// sites to inner-txn-group all pieces (live state via scratch-slot-100 +
	/// gload prologue from FunctionSplitter).
	///   subroutineName: matches awst::Subroutine::name (e.g. "RelationsLib.accumulateAuxillaryRelation").
	///   splitPoints:    statement indices; N points → N+1 pieces.
	struct HelperSplitSpec
	{
		std::string subroutineName;
		std::vector<size_t> splitPoints;
	};

	/// Lift every pure Subroutine into its own helper Contract; rewrite call
	/// sites to inner-txn the helper. Mutates `_roots` in place.
	/// `_splitSpecs` optionally triggers FunctionSplitter for multi-sidecar Subs.
	Result extract(
		std::vector<std::shared_ptr<awst::RootNode>>& _roots,
		std::vector<HelperSplitSpec> const& _splitSpecs = {});
};

} // namespace puyasol::splitter
