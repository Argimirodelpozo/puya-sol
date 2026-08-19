#pragma once

#include "awst/Node.h"

#include <cstdint>
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
	/// An EIP-1967 admin-slot use was lowered while translating the CURRENT
	/// contract's bodies: it gets the synthesized "__erc1967_admin" global and
	/// the UpdateApplication gate method (proxies/Erc1967Lowering).
	/// Snapshot-and-reset per ContractBuilder::build.
	bool usesErc1967Admin = false;
	/// AST id of the freestanding (library/free) function currently being
	/// translated, or -1 during contract translation. Freestanding bodies
	/// lower BEFORE any contract builds, so their admin-slot uses must not
	/// land in usesErc1967Admin (the first contract built would consume them).
	int64_t currentFreestandingFunctionId = -1;
	/// Freestanding function ids whose bodies lowered an admin-slot use
	/// (OZ's ERC1967Utils is a library). Unit-wide; each contract attaches
	/// the gate iff its call graph reaches one of these.
	std::set<int64_t> erc1967AdminFunctions;

	void noteErc1967AdminUse()
	{
		if (currentFreestandingFunctionId >= 0)
			erc1967AdminFunctions.insert(currentFreestandingFunctionId);
		else
			usesErc1967Admin = true;
	}

	void clear()
	{
		pendingYulSubroutines.clear();
		childContracts.clear();
		needsRipemd160 = false;
		usesErc1967Admin = false;
		currentFreestandingFunctionId = -1;
		erc1967AdminFunctions.clear();
	}
};

} // namespace puyasol::builder
