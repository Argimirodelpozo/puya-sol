#pragma once

#include "awst/Node.h"

#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Generated roots accumulated while translating nested constructs.
struct BuildArtifacts
{
	std::vector<std::shared_ptr<awst::Subroutine>> pendingYulSubroutines;
	std::set<std::string> childContracts;
	bool needsRipemd160 = false;
	/// An EIP-1967 admin-slot use was lowered: the contract gets the
	/// synthesized "__erc1967_admin" global and the bare UpdateApplication
	/// gate method (proxies/Erc1967Lowering).
	bool usesErc1967Admin = false;

	void clear()
	{
		pendingYulSubroutines.clear();
		childContracts.clear();
		needsRipemd160 = false;
		usesErc1967Admin = false;
	}
};

} // namespace puyasol::builder
