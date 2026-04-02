#pragma once

#include "awst/Node.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

#include <map>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Describes a single state variable's position in the EVM-compatible storage layout.
struct SlotVariable
{
	std::string name;          ///< Solidity variable name
	unsigned slot = 0;         ///< EVM slot number
	unsigned byteOffset = 0;   ///< Byte offset within the 32-byte slot (EVM low-order)
	unsigned byteSize = 0;     ///< Size in bytes (from Type::storageBytes())
	awst::WType const* wtype = nullptr;  ///< AWST type
	bool isFullSlot = false;   ///< True if this var occupies the entire slot alone
	int64_t declId = 0;        ///< AST declaration ID
};

/// Describes a single 32-byte storage slot and which variables are packed in it.
struct SlotInfo
{
	unsigned slotNumber = 0;
	std::vector<SlotVariable*> variables;
	unsigned bytesUsed = 0;
	bool isDynamic = false;  ///< True for mappings/arrays (box storage, not packed)
};

/// Computes the EVM-compatible storage layout for a contract's state variables.
///
/// Uses the same packing rules as Solidity's StorageOffsets:
/// - Variables pack left-to-right into 32-byte slots
/// - When a variable doesn't fit in remaining space, start a new slot
/// - Mappings and dynamic arrays always start a new slot (and use box storage)
///
/// The layout is used for:
/// - Packed global state storage on AVM (multiple small vars per slot key)
/// - Assembly sload/sstore translation (slot number → "slot_N" key)
/// - .slot and .offset resolution in inline assembly
class StorageLayout
{
public:
	/// Compute the storage layout for a contract.
	/// Walks linearized base contracts to collect all state variables.
	void computeLayout(
		solidity::frontend::ContractDefinition const& _contract,
		TypeMapper& _typeMapper
	);

	/// Look up a variable's slot info by name.
	SlotVariable const* getVarInfo(std::string const& _name) const;

	/// Look up a variable's slot info by declaration ID.
	SlotVariable const* getVarInfoById(int64_t _declId) const;

	/// Look up a slot by number.
	SlotInfo const* getSlotInfo(unsigned _slotNumber) const;

	/// Total number of slots (including dynamic/box slots).
	unsigned totalSlots() const { return m_totalSlots; }

	/// All slot infos.
	std::vector<SlotInfo> const& slots() const { return m_slots; }

	/// All variables in layout order.
	std::vector<SlotVariable> const& variables() const { return m_variables; }

	/// Generate the AVM global state key for a slot number.
	static std::string slotKey(unsigned _slot) { return "slot_" + std::to_string(_slot); }

private:
	std::vector<SlotVariable> m_variables;
	std::vector<SlotInfo> m_slots;
	std::map<std::string, size_t> m_varByName;   ///< name → index in m_variables
	std::map<int64_t, size_t> m_varById;          ///< declId → index in m_variables
	std::map<unsigned, size_t> m_slotByNumber;    ///< slotNumber → index in m_slots
	unsigned m_totalSlots = 0;
};

} // namespace puyasol::builder
