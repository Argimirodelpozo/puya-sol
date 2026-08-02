#pragma once

/// @file EvmLayoutMode.h
/// Process-global switch for `--evm-storage-layout`: contract storage is a
/// flat EVM slot space (uint256 slot → 32-byte word) backed by AVM boxes —
/// paged for the dense declared region, box-per-slot for hashed (mapping /
/// dynamic-array) slots. Set once in main, read at every storage seam.

namespace puyasol::builder
{

inline bool& evmStorageLayoutFlag()
{
	static bool enabled = false;
	return enabled;
}

inline void setEvmStorageLayout(bool _on) { evmStorageLayoutFlag() = _on; }
inline bool evmStorageLayout() { return evmStorageLayoutFlag(); }

/// --evm-memory-layout (stage 3): UNIVERSAL blob memory — every memory
/// aggregate an assembly block touches is pointer-modeled in the flat blob
/// (EVM layout), including plain locals with arbitrary initializers. The
/// selective default keeps the faster value model where asm never needs a
/// real pointer.
inline bool& evmMemoryLayoutFlag()
{
	static bool enabled = false;
	return enabled;
}

inline void setEvmMemoryLayout(bool _on) { evmMemoryLayoutFlag() = _on; }
inline bool evmMemoryLayout() { return evmMemoryLayoutFlag(); }

/// Slots below this are the DENSE region (sequential declared vars → paged
/// boxes, 64 slots per 2048-byte page = one box-ref budget). Keccak-derived
/// slots are astronomically larger, so a single runtime comparison splits the
/// regimes. 2^16 is generous: no real contract declares 65,536 top-level slots.
inline constexpr unsigned long long kEvmDenseSlotLimit = 1ULL << 16;
/// Slots per page box (64 x 32 B = 2048 B, exactly one box-reference budget).
inline constexpr unsigned kEvmSlotsPerPage = 64;

} // namespace puyasol::builder
