/// @file StorageLayout.cpp
/// Computes EVM-compatible storage layout for Solidity contracts.
/// Mirrors solidity/libsolidity/ast/Types.cpp StorageOffsets::computeOffsets().

#include "builder/storage/EvmLayoutMode.h"
#include <limits>
#include <algorithm>
#include "builder/storage/StorageLayout.h"
#include "builder/contract/StateVarWalker.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

using namespace solidity::frontend;

void StorageLayout::computeLayout(
	ContractDefinition const& _contract,
	TypeMapper& _typeMapper,
	StorageLayoutSource _source
)
{
	m_variables.clear();
	m_slots.clear();
	m_varByName.clear();
	m_varById.clear();
	m_slotByNumber.clear();
	m_totalSlots = 0;

	// `contract C layout at N`: shift base slot via storageLayoutSpecifier().baseSlot.
	solidity::u256 baseSlot = 0;
	if (auto const* spec = _contract.storageLayoutSpecifier())
		if (spec->annotation().baseSlot.set())
			baseSlot = *spec->annotation().baseSlot;
	m_contract = &_contract;
	solidity::u256 currentSlot = baseSlot;
	unsigned currentOffset = 0; // bytes used in current slot

	// ── Solidity logical layout: take solc's canonical assignment ─────────
	// `linearizedStateVariables(DataLocation::Storage)` returns solc's own
	// (declaration, slot, byteOffset) — precisely what the hand-rolled walk
	// below tries to reproduce and what the item-7 tripwire polices. Using it
	// directly removes the drift, keys everything by DECLARATION (the walk
	// de-dupes by NAME, which collapses ERC20's `string _name` onto EIP712's
	// `ShortString _name`), and inherits solc's exclusion rules for
	// constants/immutables/transients.
	//
	// Default-mode inline assembly still needs the compatibility bridge below;
	// callers select that dispatch assignment explicitly.
	if (_source == StorageLayoutSource::SolidityCanonical)
	{
		auto const* ct = solidity::frontend::TypeProvider::contract(_contract);
		for (auto const& [decl, slot, offset]:
			ct->linearizedStateVariables(solidity::frontend::DataLocation::Storage))
		{
			if (!decl || !decl->type())
				continue;
			SlotVariable sv;
			sv.name = decl->name();
			// solc's linearizedStateVariables already folds the `layout at N`
			// base in (computeOffsets with layoutBaseForInheritanceHierarchy);
			// adding baseSlot again DOUBLED it (x.slot returned 14 for
			// `layout at 7`).
			sv.slot = slot;
			sv.byteOffset = offset;
			sv.byteSize = decl->type()->storageBytes();
			sv.wtype = _typeMapper.map(decl->type());
			sv.solType = decl->type();
			auto span = decl->type()->storageSize();
			sv.isFullSlot = (sv.byteSize == 32) || span > 1;
			sv.declId = decl->id();

			size_t idx = m_variables.size();
			m_variables.push_back(sv);
			// name map keeps the FIRST declaration (asm `.slot` by name has no
			// better answer); the id map is the unambiguous one.
			m_varByName.emplace(sv.name, idx);
			m_varById[sv.declId] = idx;

			if (m_slotByNumber.find(sv.slot) == m_slotByNumber.end())
			{
				SlotInfo si;
				si.slotNumber = sv.slot;
				auto const* at = dynamic_cast<ArrayType const*>(decl->type());
				si.isDynamic = dynamic_cast<MappingType const*>(decl->type())
					|| (at && at->isDynamicallySized());
				m_slotByNumber[sv.slot] = m_slots.size();
				m_slots.push_back(si);
			}
			auto si = m_slotByNumber[sv.slot];
			m_slots[si].variableIndices.push_back(idx);
			m_slots[si].bytesUsed = std::max<unsigned>(
				m_slots[si].bytesUsed, sv.byteOffset + sv.byteSize);

			auto end = sv.slot + (span > 1 ? span : 1) - baseSlot;
			if (end > solidity::u256(m_totalSlots))
				m_totalSlots = end > solidity::u256(
						std::numeric_limits<unsigned>::max())
					? std::numeric_limits<unsigned>::max()
					: static_cast<unsigned>(end);
		}
		Logger::instance().debug(
			"Storage layout (solc canonical): "
			+ std::to_string(m_variables.size()) + " variables in "
			+ std::to_string(m_totalSlots) + " slots",
			awst::SourceLocation{});
		return;
	}

	// Collect state vars base-first (reverse of linearization).
	std::vector<VariableDeclaration const*> allVars;
	forEachStateVarReverse(_contract, [&](auto const* var)
	{
		if (var->isConstant() || var->immutable())
			return;
		// Transient vars (EIP-1153) have an independent namespace; TransientStorage handles them.
		if (var->referenceLocation() == VariableDeclaration::Location::Transient)
			return;
		bool alreadySeen = false; // de-dup inherited vars
		for (auto const* existing: allVars)
			if (existing->name() == var->name()) { alreadySeen = true; break; }
		if (alreadySeen) return;
		allVars.push_back(var);
	});

	for (auto const* var: allVars)
	{
		auto const* solType = var->type();
		unsigned byteSize = 32; // default for unknown types
		// FULL-width span: a denomination-sized array (`uint[2 ether]`) spans
		// ~2e18 slots — the old `unsigned` clamp saturated it at 2^32-1 and
		// shifted every FOLLOWING var to a wrong slot (caught by the item-7
		// solc-layout tripwire on its first corpus run).
		solidity::u256 slotsSpanned = 1;

		// storageBytes() = byte width within a slot; storageSize() = slot count.
		if (solType)
		{
			byteSize = solType->storageBytes();
			slotsSpanned = solType->storageSize();
			if (slotsSpanned == 0)
				slotsSpanned = 1;
		}

		bool isDynamic = false;
		if (dynamic_cast<MappingType const*>(solType))
			isDynamic = true;
		if (auto const* arr = dynamic_cast<ArrayType const*>(solType))
			if (arr->isDynamicallySized())
				isDynamic = true;

		bool isMultiSlot = (slotsSpanned > 1) || isDynamic;

		// Multi-slot types always align to a fresh slot; single-slot types pack
		// into the current slot if they fit.
		if (isMultiSlot || currentOffset + byteSize > 32)
		{
			if (currentOffset > 0)
				currentSlot++;
			currentOffset = 0;
		}

		// Record.
		SlotVariable sv;
		sv.name = var->name();
		sv.slot = currentSlot;
		sv.byteOffset = currentOffset;
		sv.byteSize = byteSize;
		sv.wtype = _typeMapper.map(solType);
		sv.solType = solType;
		sv.isFullSlot = (byteSize == 32) || isMultiSlot;
		sv.declId = var->id();

		size_t varIdx = m_variables.size();
		m_variables.push_back(sv);
		m_varByName[sv.name] = varIdx;
		m_varById[sv.declId] = varIdx;

		// Ensure slot record exists.
		if (m_slotByNumber.find(currentSlot) == m_slotByNumber.end())
		{
			SlotInfo si;
			si.slotNumber = currentSlot;
			si.isDynamic = isDynamic;
			size_t slotIdx = m_slots.size();
			m_slots.push_back(si);
			m_slotByNumber[currentSlot] = slotIdx;
		}

		// Add variable to slot record.
		auto slotIdx = m_slotByNumber[currentSlot];
		m_slots[slotIdx].variableIndices.push_back(varIdx);
		m_slots[slotIdx].bytesUsed = currentOffset + byteSize;

		// Advance position.
		if (isMultiSlot)
		{
			currentSlot += slotsSpanned;
			currentOffset = 0;
		}
		else
		{
			currentOffset += byteSize;
		}
	}

	// Relative used-slot count (layout-at bases would otherwise overflow this).
	solidity::u256 used = currentSlot - baseSlot + ((currentOffset > 0) ? 1 : 0);
	m_totalSlots = used > std::numeric_limits<unsigned>::max()
		? std::numeric_limits<unsigned>::max()
		: static_cast<unsigned>(used);

	Logger::instance().debug(
		"Storage layout: " + std::to_string(m_variables.size()) + " variables in "
		+ std::to_string(m_totalSlots) + " slots",
		awst::SourceLocation{}
	);

	// Differential tripwire (possible_solc item 7): our packing mirrors
	// Solidity's StorageOffsets rules BY HAND — compare every var's
	// (slot, byteOffset) against solc's own canonical assignment and
	// hard-error on drift. Every compile of every fixture is now a layout
	// differential; a trip means OUR packing walk diverged — report it.
	{
		auto const* ct = solidity::frontend::TypeProvider::contract(_contract);
		for (auto const& [decl, slot, offset]:
			ct->linearizedStateVariables(solidity::frontend::DataLocation::Storage))
		{
			if (!decl)
				continue;
			auto const* ours = getVarInfoById(decl->id());
			if (!ours)
				continue; // vars we place elsewhere (boxes) — skip, not a drift
			if (ours->slot != slot || ours->byteOffset != offset)
				Logger::instance().error(
					"internal storage-layout drift for `" + decl->name()
						+ "`: puya-sol computed slot " + ours->slot.str()
						+ " offset " + std::to_string(ours->byteOffset)
						+ " but solc's canonical layout says slot " + slot.str()
						+ " offset " + std::to_string(offset)
						+ " — the hand-mirrored packing walk diverged; report this.",
					awst::SourceLocation{});
		}
	}
}

SlotVariable const* StorageLayout::getVarInfo(std::string const& _name) const
{
	auto it = m_varByName.find(_name);
	return (it != m_varByName.end()) ? &m_variables[it->second] : nullptr;
}

SlotVariable const* StorageLayout::getVarInfoById(int64_t _declId) const
{
	auto it = m_varById.find(_declId);
	return (it != m_varById.end()) ? &m_variables[it->second] : nullptr;
}

SlotInfo const* StorageLayout::getSlotInfo(solidity::u256 const& _slotNumber) const
{
	auto it = m_slotByNumber.find(_slotNumber);
	return (it != m_slotByNumber.end()) ? &m_slots[it->second] : nullptr;
}

} // namespace puyasol::builder
