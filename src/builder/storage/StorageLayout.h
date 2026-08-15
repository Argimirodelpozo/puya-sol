#pragma once

#include "awst/Node.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace puyasol::builder
{

/// Which slot assignment a StorageLayout describes. SolidityCanonical is the
/// language-level layout reported by solc. LegacyDispatch is only the bridge
/// used by default-mode sload/sstore routing to the frontend's named AVM cells.
/// Keeping these explicit prevents physical AVM placement from being mistaken
/// for Solidity's logical slot space.
enum class StorageLayoutSource
{
	SolidityCanonical,
	LegacyDispatch,
};

/// Describes a single state variable's position in the EVM-compatible storage layout.
struct SlotVariable
{
	std::string name;          ///< Solidity variable name
	solidity::u256 slot = 0;   ///< EVM slot number (layout-at bases can be near 2^256)
	unsigned byteOffset = 0;   ///< Byte offset within the 32-byte slot (EVM low-order)
	unsigned byteSize = 0;     ///< Size in bytes (from Type::storageBytes())
	awst::WType const* wtype = nullptr;  ///< AWST type
	solidity::frontend::Type const* solType = nullptr;  ///< Solidity type (signedness etc.)
	bool isFullSlot = false;   ///< True if this var occupies the entire slot alone
	int64_t declId = 0;        ///< AST declaration ID
};

/// Describes a single 32-byte storage slot and which variables are packed in it.
struct SlotInfo
{
	solidity::u256 slotNumber = 0;
	/// Indices into StorageLayout::variables(). Indices remain valid while the
	/// backing vector grows; pointers into it would not.
	std::vector<size_t> variableIndices;
	unsigned bytesUsed = 0;
	bool isDynamic = false;  ///< True for mappings/arrays (box storage, not packed)
};

/// EVM-compatible storage layout (mirrors Solidity's StorageOffsets packing rules).
/// Used for: AVM global-state packing, sload/sstore → slot_N key translation,
/// and .slot/.offset resolution in inline assembly.
class StorageLayout
{
public:
	/// Compute the selected slot assignment for a contract.
	void computeLayout(
		solidity::frontend::ContractDefinition const& _contract,
		TypeMapper& _typeMapper,
		StorageLayoutSource _source
	);

	/// The contract this layout was computed for (nullptr before compute).
	/// Used to tell "our var is missing" (a real layout bug) apart from "this
	/// declaration belongs to ANOTHER contract" (a foreign reference some pass
	/// speculatively lowered and will discard).
	solidity::frontend::ContractDefinition const* contract() const
	{
		return m_contract;
	}

	/// Look up a variable's slot info by name.
	SlotVariable const* getVarInfo(std::string const& _name) const;

	/// Look up a variable's slot info by declaration ID.
	SlotVariable const* getVarInfoById(int64_t _declId) const;

	/// Look up a slot by number.
	SlotInfo const* getSlotInfo(solidity::u256 const& _slotNumber) const;

	/// Number of USED slots (relative count — layout-at bases don't inflate it).
	unsigned totalSlots() const { return m_totalSlots; }

	/// All slot infos.
	std::vector<SlotInfo> const& slots() const { return m_slots; }

	/// All variables in layout order.
	std::vector<SlotVariable> const& variables() const { return m_variables; }

private:
	std::vector<SlotVariable> m_variables;
	std::vector<SlotInfo> m_slots;
	std::map<std::string, size_t> m_varByName;   ///< name → index in m_variables
	std::map<int64_t, size_t> m_varById;          ///< declId → index in m_variables
	std::map<solidity::u256, size_t> m_slotByNumber;   ///< slotNumber → index in m_slots
	unsigned m_totalSlots = 0;
	solidity::frontend::ContractDefinition const* m_contract = nullptr;
};

} // namespace puyasol::builder
