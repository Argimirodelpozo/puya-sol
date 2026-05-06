#pragma once

/// @file PureHelperExtractor.h
/// Extract pure Subroutines into standalone "helper" Contracts that are
/// deployed as separate apps and called via inner-txn ApplicationCall.
///
/// Why: large pure helpers (math-heavy, no state access) bloat every
/// chunk that transitively reaches them. Lifting each pure Sub into
/// its own one-method Contract removes its bytecode from the original
/// program (puya DCE'd post-rewrite), at the cost of one inner-txn per
/// call. State-coordination is trivial because the helpers are pure —
/// args in, ARC4-encoded return value out via the standard `log` ABI
/// convention; no rekey, no auth, no shared globals.
///
/// Pipeline placement: AFTER --fn-split (so any pieces are already
/// formed) and BEFORE --uros-splitter (so uros's roots already include
/// the helper Contracts and exclude the rewritten Subs).

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
		/// Original Subroutine ID that got lifted (no longer reachable
		/// from non-helper roots after the rewrite).
		std::string subId;

		/// Synthesized helper Contract ID. The Contract's only role is
		/// to host an approval program that ABI-routes one selector to
		/// the lifted Subroutine's body.
		std::string helperContractId;

		/// Template variable name baked into the rewritten call sites.
		/// The deploy harness substitutes the helper's app id at deploy
		/// time, the same way --uros-splitter substitutes
		/// TMPL_UROS_ORCH_APP_ID.
		std::string templateVarName;

		/// 4-byte ARC4 selector (sha512_256(canonicalSig)[:4]) of the
		/// helper's one ABI method. Both the rewritten call sites
		/// (encoded as ApplicationArgs[0]) and the helper's approval
		/// router check this value.
		std::vector<uint8_t> selector;
	};

	struct Result
	{
		std::vector<std::shared_ptr<awst::Contract>> helperContracts;
		std::vector<ExtractedHelper> extracted;
		bool didExtract = false;
	};

	/// Lift every pure Subroutine reachable from `_roots` into its own
	/// helper Contract; rewrite call sites in every body to inner-txn
	/// the helper. Mutates `_roots` in place: removes the lifted
	/// Subroutines, appends the new helper Contracts.
	Result extract(std::vector<std::shared_ptr<awst::RootNode>>& _roots);
};

} // namespace puyasol::splitter
