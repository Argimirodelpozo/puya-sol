/// @file StorageLayout.cpp
/// Computes EVM-compatible storage layout for Solidity contracts.
/// Mirrors solidity/libsolidity/ast/Types.cpp StorageOffsets::computeOffsets().

#include "builder/storage/StorageLayout.h"
#include "builder/contract/StateVarWalker.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

using namespace solidity::frontend;

void StorageLayout::computeLayout(
	ContractDefinition const& _contract,
	TypeMapper& _typeMapper
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
	solidity::u256 currentSlot = baseSlot;
	unsigned currentOffset = 0; // bytes used in current slot

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

	// Reserve upfront: m_slots[].variables hold &m_variables[i]; a mid-loop
	// reallocation would dangle those pointers.
	m_variables.reserve(allVars.size());

	for (auto const* var: allVars)
	{
		auto const* solType = var->type();
		unsigned byteSize = 32; // default for unknown types
		unsigned slotsSpanned = 1;

		// storageBytes() = byte width within a slot; storageSize() = slot count.
		if (solType)
		{
			byteSize = solType->storageBytes();
			solidity::u256 size = solType->storageSize();
			if (size > std::numeric_limits<unsigned>::max())
				slotsSpanned = std::numeric_limits<unsigned>::max();
			else
				slotsSpanned = static_cast<unsigned>(size);
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
		m_slots[slotIdx].variables.push_back(&m_variables[varIdx]);
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
