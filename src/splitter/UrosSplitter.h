#pragma once

/// @file UrosSplitter.h
///
/// **`--uros-splitter` technique** — split named functions out of an
/// over-large contract by producing TWO complete contract bytecodes plus an
/// orchestrator app that swaps between them per call.
///
/// Layout:
///
///   `main`     — full contract surface, but split functions have STUB
///                bodies (no-op `int 1; return`). Real bodies for every
///                kept function. ABI selectors, state var schema, and
///                constructor remain intact.
///
///   `helper`   — same contract surface as `main`, but flipped: STUB
///                bodies for kept functions, REAL bodies for split
///                functions. Constructor body stubbed (helper is never
///                deployed, only spliced in via UpdateApplication).
///                Same state-var schema as `main`, so storage slots/keys
///                match across the swap.
///
///   `orchestrator` (generated separately, hand-written algopy) —
///                holds `__codebox_0` (main bytes) + `__codebox_1`
///                (helper bytes) in box storage; its single dispatcher
///                method submits an itxn group:
///                  1. UpdateApplication on `main` with helper bytes
///                  2. ApplicationCall on `main` with the user-passed
///                     selector + args (now executing helper code
///                     against main's storage)
///                  3. UpdateApplication on `main` with main bytes
///                     (restore original)
///
/// User submits a group `[stub_call_to_main, dispatch_call_to_orchestrator]`.
/// The stub call is a no-op that just delivers the args through
/// gtxn[N-1].ApplicationArgs; the orchestrator reads them and runs the dance.
///
/// This splitter only emits the AWST sides — the orchestrator is a separate
/// artifact (template at `src/splitter/uros_orchestrator.py.in`) that the
/// caller compiles via puyapy.

#include "awst/Node.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::splitter
{

class UrosSplitter
{
public:
	struct Result
	{
		/// Roots that go through the normal puya backend pipeline to
		/// produce the live `MyContract.approval.bin`. Split methods are
		/// stubbed.
		std::vector<std::shared_ptr<awst::RootNode>> mainRoots;

		/// Roots that produce the swap-in `MyContract__split.approval.bin`.
		/// Kept methods are stubbed; split methods are real. Same state-var
		/// schema, same selectors, same ABI shape.
		std::vector<std::shared_ptr<awst::RootNode>> helperRoots;

		/// Names actually applied (intersection of the user's list with
		/// methods present in the AWST). Logged in the splitter's diagnostic
		/// output and exposed for downstream tooling that needs to know
		/// which selectors get the dance treatment.
		std::vector<std::string> appliedNames;
	};

	/// Split `_roots` based on `_splitNames`. Returns Result with two parallel
	/// root sets ready for the puya backend.
	///
	/// `_splitNames` is treated as a set of `memberName` matches against the
	/// primary contract's `methods`. Names not found are reported as warnings
	/// and dropped (so one config can target multiple contract families).
	///
	/// Subroutines are duplicated into both root sets unchanged — the splitter
	/// does no call-graph analysis. Most of the size win is from method bodies
	/// anyway; subroutine deduplication is a follow-up optimisation.
	static Result split(
		std::vector<std::shared_ptr<awst::RootNode>> const& _roots,
		std::set<std::string> const& _splitNames);
};

} // namespace puyasol::splitter
