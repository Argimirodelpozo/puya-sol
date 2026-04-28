#pragma once

#include "awst/Node.h"

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
/// individual functions still fit in 8KB and you just need to relocate a few
/// large utilities to a helper.
///
/// The helper's app id is sourced from a TemplateVar (`TMPL_<helperName>_APP_ID`)
/// resolved at deploy time.
class SimpleSplitter
{
public:
	struct ContractAWST
	{
		std::string contractId;
		std::string contractName;
		std::vector<std::shared_ptr<awst::RootNode>> roots;
	};

	/// Move subroutines named in `_moveNames` to a helper contract. Returns
	/// two ContractAWSTs: helper first, then orchestrator. If no matching
	/// subroutines are found in `_roots`, returns empty.
	std::vector<ContractAWST> split(
		std::vector<std::shared_ptr<awst::RootNode>> const& _roots,
		std::vector<std::string> const& _moveNames,
		int _helperIndex
	);
};

} // namespace puyasol::splitter
