/// @file StorageLayout.cpp
/// Loads solc's canonical storage layout for Solidity contracts.

#include <limits>
#include <algorithm>
#include "builder/storage/StorageLayout.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>
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
	m_varById.clear();
	m_slotByNumber.clear();
	m_totalSlots = 0;

	// `contract C layout at N`: shift base slot via storageLayoutSpecifier().baseSlot.
	solidity::u256 baseSlot = 0;
	if (auto const* spec = _contract.storageLayoutSpecifier())
		if (spec->annotation().baseSlot.set())
			baseSlot = *spec->annotation().baseSlot;
	m_contract = &_contract;

	// `linearizedStateVariables(DataLocation::Storage)` is solc's authoritative
	// (declaration, slot, byteOffset) assignment. Both AVM storage modes consume
	// this same logical layout; only their physical declaration-to-cell binding
	// differs.
	auto const* ct = solidity::frontend::TypeProvider::contract(_contract);
	for (auto const& [decl, slot, offset]:
		ct->linearizedStateVariables(solidity::frontend::DataLocation::Storage))
	{
		if (!decl || !decl->type())
			continue;
		SlotVariable sv;
		sv.name = decl->name();
		// solc already folds the `layout at N` base into each slot.
		sv.slot = slot;
		sv.byteOffset = offset;
		sv.byteSize = decl->type()->storageBytes();
		// Logical slot/length facts remain valid for huge sparse arrays even
		// when no complete ARC4 buffer could represent their declared value.
		sv.wtype = _typeMapper.tryMapStorageRepresentation(decl->type());
		sv.solType = decl->type();
		auto span = decl->type()->storageSize();
		sv.isFullSlot = (sv.byteSize == 32) || span > 1;
		sv.declId = decl->id();
		sv.declaration = decl;

		size_t idx = m_variables.size();
		m_variables.push_back(sv);
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
		auto slotIndex = m_slotByNumber[sv.slot];
		m_slots[slotIndex].variableIndices.push_back(idx);
		m_slots[slotIndex].bytesUsed = std::max<unsigned>(
			m_slots[slotIndex].bytesUsed, sv.byteOffset + sv.byteSize);

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
