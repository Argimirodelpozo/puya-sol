/// @file StorageLayout.cpp
/// Computes EVM-compatible storage layout for Solidity contracts.
/// Mirrors solidity/libsolidity/ast/Types.cpp StorageOffsets::computeOffsets().

#include "builder/storage/StorageLayout.h"
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

	// `contract C layout at N` shifts the base storage slot to N. When the
	// annotation is present, Solidity stores the evaluated base in
	// storageLayoutSpecifier()->annotation().baseSlot (a SetOnce).
	unsigned baseSlot = 0;
	if (auto const* spec = _contract.storageLayoutSpecifier())
	{
		if (spec->annotation().baseSlot.set())
		{
			auto const& v = *spec->annotation().baseSlot;
			if (v <= std::numeric_limits<unsigned>::max())
				baseSlot = static_cast<unsigned>(v);
		}
	}
	unsigned currentSlot = baseSlot;
	unsigned currentOffset = 0; // bytes used in current slot

	// Walk linearized base contracts (most-base first = reverse of linearization)
	auto const& linearized = _contract.annotation().linearizedBaseContracts;
	std::vector<VariableDeclaration const*> allVars;

	// Collect in correct order: base-first
	for (auto it = linearized.rbegin(); it != linearized.rend(); ++it)
	{
		for (auto const* var: (*it)->stateVariables())
		{
			if (var->isConstant() || var->immutable())
				continue;
			// Skip if already seen (inherited and not overridden)
			bool alreadySeen = false;
			for (auto const* existing: allVars)
				if (existing->name() == var->name()) { alreadySeen = true; break; }
			if (alreadySeen) continue;
			allVars.push_back(var);
		}
	}

	for (auto const* var: allVars)
	{
		auto const* solType = var->type();
		unsigned byteSize = 32; // default for unknown types

		// Use Solidity's storageBytes() for accurate sizing
		if (solType)
			byteSize = solType->storageBytes();

		// Dynamic types (mappings, dynamic arrays) always start a new slot
		// and consume a full slot (the actual data is in a derived location)
		bool isDynamic = false;
		if (dynamic_cast<MappingType const*>(solType) ||
			dynamic_cast<ArrayType const*>(solType))
		{
			isDynamic = true;
			byteSize = 32; // base slot is always 32 bytes
		}

		// Check if we need a new slot
		if (isDynamic || byteSize > 32 || currentOffset + byteSize > 32)
		{
			// Start a new slot (if current slot has content)
			if (currentOffset > 0)
				currentSlot++;
			currentOffset = 0;
		}

		// Record the variable
		SlotVariable sv;
		sv.name = var->name();
		sv.slot = currentSlot;
		sv.byteOffset = currentOffset;
		sv.byteSize = byteSize;
		sv.wtype = _typeMapper.map(solType);
		sv.isFullSlot = (byteSize == 32) || isDynamic;
		sv.declId = var->id();

		size_t varIdx = m_variables.size();
		m_variables.push_back(sv);
		m_varByName[sv.name] = varIdx;
		m_varById[sv.declId] = varIdx;

		// Ensure slot exists
		if (m_slotByNumber.find(currentSlot) == m_slotByNumber.end())
		{
			SlotInfo si;
			si.slotNumber = currentSlot;
			si.isDynamic = isDynamic;
			size_t slotIdx = m_slots.size();
			m_slots.push_back(si);
			m_slotByNumber[currentSlot] = slotIdx;
		}

		// Add variable to slot
		auto slotIdx = m_slotByNumber[currentSlot];
		m_slots[slotIdx].variables.push_back(&m_variables[varIdx]);
		m_slots[slotIdx].bytesUsed = currentOffset + byteSize;

		currentOffset += byteSize;

		// Dynamic types consume the full slot, advance
		if (isDynamic)
		{
			currentSlot++;
			currentOffset = 0;
		}
	}

	m_totalSlots = (currentOffset > 0) ? currentSlot + 1 : currentSlot;

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

SlotInfo const* StorageLayout::getSlotInfo(unsigned _slotNumber) const
{
	auto it = m_slotByNumber.find(_slotNumber);
	return (it != m_slotByNumber.end()) ? &m_slots[it->second] : nullptr;
}

} // namespace puyasol::builder
