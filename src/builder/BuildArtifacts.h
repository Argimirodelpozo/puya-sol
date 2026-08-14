#pragma once

#include "awst/Node.h"

#include <memory>
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

	void clear()
	{
		pendingYulSubroutines.clear();
		childContracts.clear();
		needsRipemd160 = false;
	}
};

} // namespace puyasol::builder
