#pragma once

#include <stdexcept>
#include <vector>

namespace puyasol::builder
{

/// Immutable scratch-space allocation for one compilation.
///
/// EVM memory pages are CONTIGUOUS from slot 0 — always slots 0..N-1 — and the
/// reservations that used to be absolute follow right behind them: the
/// EIP-1153 transient blob at slot N, and the ten AVM.sol group-scratch
/// ("flash accounting") slots at N+1..N+10. With the default N=5 this is
/// byte-identical to the historical fixed layout (pages 0..4, transient 5,
/// flash 6..15).
///
/// The old model instead relocated pages WHOLESALE to slots 16.. once N
/// exceeded 5, wasting 0..4 and putting a discontinuity in the middle of the
/// address computation; the relocated range never worked against a real
/// workload (see memory note evm-memory-slots-16-bug).
///
/// Group-scratch caveat: `Scratch.load` reads ANOTHER txn's slots by number,
/// so contracts sharing a group-level slot convention must be compiled with
/// the same N (the default keeps today's numbers). Nothing in-repo pins the
/// flash range beyond documentation; the compiler itself never emits it.
///
/// The splitter's own cross-chunk slots start at
/// FunctionSplitter::kLiveVarsScratchSlot (100); the constructor bound keeps
/// every layout slot clear of them.
class ScratchLayout
{
public:
	static constexpr int slotSize = 4096;
	static constexpr int flashSlotCount = 10;
	static constexpr int maxScratchSlot = 255;
	static constexpr int defaultMemorySlots = 5;
	/// flashLast() must stay below the splitter's slot range (100).
	static constexpr int maxMemorySlots = 88;

	explicit ScratchLayout(int _memorySlots = defaultMemorySlots)
		: m_memorySlots(_memorySlots)
	{
		if (_memorySlots < 1 || _memorySlots > maxMemorySlots)
			throw std::invalid_argument("EVM memory scratch-slot count is out of range");
	}

	int memoryFirst() const { return 0; }
	int memoryLast() const { return m_memorySlots - 1; }
	int memoryCount() const { return m_memorySlots; }
	int transientSlot() const { return m_memorySlots; }
	int flashFirst() const { return m_memorySlots + 1; }
	int flashLast() const { return m_memorySlots + flashSlotCount; }

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
		result.reserve(static_cast<size_t>(m_memorySlots + 1 + flashSlotCount));
		result.push_back(transientSlot());
		for (int slot = flashFirst(); slot <= flashLast(); ++slot)
			result.push_back(slot);
		return result;
	}

private:
	int m_memorySlots;
};

} // namespace puyasol::builder
