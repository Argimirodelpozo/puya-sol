#pragma once
#include <libsolutil/Common.h>

#include "awst/Node.h"

#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace solidity::frontend
{
class Type;
class FunctionDefinition;
}

namespace puyasol::builder
{

/// Generated roots accumulated while translating nested constructs.
struct BuildArtifacts
{
	std::vector<std::shared_ptr<awst::Subroutine>> pendingYulSubroutines;
	/// Shared scratch-memory and byte-buffer primitives; unit-wide.
	std::map<std::string, std::shared_ptr<awst::Subroutine>> bufferSubroutines;
	/// --evm-storage-layout: RECURSIVE struct types (S{S[] x}) cannot inline
	/// their delete — clearing recurses through a per-type runtime subroutine
	/// instead. Canonical type identifier -> unit-global SubroutineID target;
	/// entries whose bodies are not yet synthesized sit in the pending queue
	/// (drained by EvmSlotLowering into pendingYulSubroutines).
	std::map<std::string, std::string> evmClearSubs;
	std::vector<std::pair<std::string, solidity::frontend::Type const*>> pendingEvmClearSubs;
	std::set<std::string> childContracts;

	struct ContractEmission
	{
		std::set<std::string> boxProvisionedChildren;
		std::map<std::string, std::string> helpers;
		std::vector<awst::ContractMethod> pendingHelpers;
		bool usesErc1967Admin = false;
	};
	/// Stack-owned emission state; recursive/nested builds restore their host.
	class ContractScope
	{
	public:
		explicit ContractScope(BuildArtifacts& owner)
			: m_scope(owner.m_contract, &m_value) {}
		ContractScope(ContractScope const&) = delete;
		ContractScope& operator=(ContractScope const&) = delete;
	private:
		ContractEmission m_value;
		solidity::ScopedSaveAndRestore<ContractEmission*> m_scope;
	};
	ContractEmission& contract()
	{
		if (!m_contract) throw std::logic_error("Contract emission requested outside a contract build");
		return *m_contract;
	}

	/// Interior aggregate storage references passed by reference (OpenZeppelin
	/// `Checkpoints.push(Trace storage self)` → `_insert(self._checkpoints, …)`):
	/// the library/free callee is specialized per (function, parameter field
	/// paths) so the parameter aliases `FieldExpression(box(key), path…)` of the
	/// enclosing box instead of a whole box. Requested at call sites, built
	/// after translation (a specialized body may request more).
	struct PathSpecialization
	{
		struct Param
		{
			size_t index = 0;
			std::vector<std::string> path;
			awst::WType const* enclosingWType = nullptr;
		};
		solidity::frontend::FunctionDefinition const* function = nullptr;
		std::vector<Param> params;
		std::string id;
	};
	std::map<std::string, std::string> pathSpecializationIds;
	std::vector<PathSpecialization> pendingPathSpecializations;
	bool needsRipemd160 = false;
	/// Shared by outlined Yul helpers and their Solidity host frame. Unit-wide
	/// so uses in freestanding functions also reserve and initialize the slot.
	bool usesReturnData = false;
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
			contract().usesErc1967Admin = true;
	}

	void clear()
	{
		if (m_contract) throw std::logic_error("Cannot reset artifacts during a contract build");
		*this = {};
	}

private:
	ContractEmission* m_contract = nullptr;
};

} // namespace puyasol::builder
