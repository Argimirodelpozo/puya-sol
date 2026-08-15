#pragma once

#include <stdexcept>
#include <vector>

namespace puyasol::builder
{

/// Immutable scratch-space allocation for one compilation.
///
/// Slots 5..15 are ABI-visible reservations: 5 holds EIP-1153 transient
/// storage and 6..15 are used by AVM.sol flash accounting. Small EVM-memory
/// configurations occupy 0..4; larger ones move in their entirety above the
/// reserved range.
class ScratchLayout
{
public:
	static constexpr int slotSize = 4096;
	static constexpr int transientSlot = 5;
	static constexpr int flashFirst = 6;
	static constexpr int flashLast = 15;
	static constexpr int extendedMemoryFirst = 16;
	static constexpr int maxScratchSlot = 255;
	static constexpr int defaultMemorySlots = 5;

	explicit ScratchLayout(int _memorySlots = defaultMemorySlots)
		: m_memorySlots(_memorySlots),
		  m_memoryFirst(_memorySlots <= defaultMemorySlots ? 0 : extendedMemoryFirst)
	{
		if (_memorySlots < 1
			|| m_memoryFirst + _memorySlots - 1 > maxScratchSlot)
			throw std::invalid_argument("EVM memory scratch-slot count is out of range");
	}

	int memoryFirst() const { return m_memoryFirst; }
	int memoryLast() const { return m_memoryFirst + m_memorySlots - 1; }
	int memoryCount() const { return m_memorySlots; }
	std::vector<int> memorySlots() const
	{
		std::vector<int> result;
		result.reserve(static_cast<size_t>(m_memorySlots));
		for (int slot = memoryFirst(); slot <= memoryLast(); ++slot)
			result.push_back(slot);
		return result;
	}

	std::vector<int> reservedSlots() const
	{
		auto result = memorySlots();
		result.reserve(static_cast<size_t>(m_memorySlots + 11));
		result.push_back(transientSlot);
		for (int slot = flashFirst; slot <= flashLast; ++slot)
			result.push_back(slot);
		return result;
	}

private:
	int m_memorySlots;
	int m_memoryFirst;
};

} // namespace puyasol::builder
