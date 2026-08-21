#pragma once

#include "awst/Node.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puyasol::splitter
{

/// Static "extract-named-subroutines" splitter. Moves whole subroutines from
/// the primary contract into a sibling helper contract; the orchestrator
/// keeps a local stub for each moved subroutine that inner-app-calls the
/// helper and returns the decoded result. All call sites in the orchestrator
/// continue to invoke the local subroutine — the round-trip is hidden inside.
///
/// This is the simplest split mode: no function-body chopping, no orchestrator
/// transformation beyond replacing the moved subroutines' bodies. Use when
/// individual functions still fit in 16 KiB and you just need to relocate a few
/// large utilities to a helper.
///
/// The helper's app id is sourced from a TemplateVar (`TMPL_<helperName>_APP_ID`)
/// resolved at deploy time.
///
/// In delegate mode, externally routable methods are also emitted as compiled
/// code pages. A caller can install a page on the original app, execute with
/// the original app identity/storage/sender context, then restore the main
/// program. Both programs expose the UpdateApplication hatch needed for that
/// transition.
class SimpleSplitter
{
public:
	struct ContractAWST
	{
		std::string contractId;
		std::string contractName;
		std::vector<std::shared_ptr<awst::RootNode>> roots;
	};

	/// Move subroutines in `_moveNames` to a helper contract. Returns
	/// [helper, orchestrator]; empty if no matching subroutines are found.
	///
	/// `_ensureBudget`: per-method opcode-budget targets. When a helper
	/// method's name matches a key, prepends `puya_lib::ensure_budget(N)`
	/// to its body (bypasses ContractBuilder.cpp's per-method injection).
	/// `_delegateMode` emits UpdateApplication routes on the page and main.
	std::vector<ContractAWST> split(
		std::vector<std::shared_ptr<awst::RootNode>> const& _roots,
		std::vector<std::string> const& _moveNames,
		int _helperIndex,
		std::map<std::string, uint64_t> const& _ensureBudget = {},
		bool _delegateMode = false
	);
};

} // namespace puyasol::splitter
