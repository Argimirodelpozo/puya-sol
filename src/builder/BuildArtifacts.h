#pragma once

#include "awst/Node.h"

#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace solidity::frontend
{
class Type;
}

namespace puyasol::builder
{

/// Generated roots accumulated while translating nested constructs.
struct BuildArtifacts
{
	std::vector<std::shared_ptr<awst::Subroutine>> pendingYulSubroutines;
	/// --evm-storage-layout: RECURSIVE struct types (S{S[] x}) cannot inline
	/// their delete — clearing recurses through a per-type runtime subroutine
	/// instead. Canonical type identifier -> unit-global SubroutineID target;
	/// entries whose bodies are not yet synthesized sit in the pending queue
	/// (drained by EvmSlotLowering into pendingYulSubroutines).
	std::map<std::string, std::string> evmClearSubs;
	std::vector<std::pair<std::string, solidity::frontend::Type const*>> pendingEvmClearSubs;
	std::set<std::string> childContracts;
	/// Children whose `new C()` create loads the approval program from a
	/// "__cp_<Child>" box (--child-programs-via-box). The contract that owns
	/// such creates gets the synthesized __provisionChildProg method.
	/// Snapshot-and-reset per ContractBuilder::build (like usesErc1967Admin).
	std::set<std::string> boxProvisionedChildren;
	/// EVM-router calldata decoders memoized per struct: canonical struct id
	/// -> contract-method name (`__evm_decs_<id>`), bodies queued here and
	/// appended to the contract after dispatch is built. Per-contract.
	std::map<std::string, std::string> evmDecodeStructMethods;
	std::vector<awst::ContractMethod> pendingEvmDecodeMethods;
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
		evmClearSubs.clear();
		pendingEvmClearSubs.clear();
		childContracts.clear();
		boxProvisionedChildren.clear();
		evmDecodeStructMethods.clear();
		pendingEvmDecodeMethods.clear();
		needsRipemd160 = false;
		usesErc1967Admin = false;
		currentFreestandingFunctionId = -1;
		erc1967AdminFunctions.clear();
	}
};

} // namespace puyasol::builder
