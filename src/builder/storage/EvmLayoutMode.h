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

/// COMPILE-UNIT-global dense-only decision (slot mode): the storage runtime
/// subroutines (`__storage_read/write`, …) share ONE SubroutineID across every
/// contract in the unit, so puya links a single body — per-contract variants
/// silently clobber each other (a stateless helper's slim body replacing a
/// mapping contract's full one crashed `btoi` on keccak slots). AWSTBuilder
/// pre-scans ALL contracts and sets these ONCE; the dispatch builder only
/// reads them. Defaults are the safe full-fat bodies.
inline bool& evmDenseOnlyUnitFlag()
{
	static bool denseOnly = false;
	return denseOnly;
}
inline void setEvmDenseOnlyUnit(bool _on) { evmDenseOnlyUnitFlag() = _on; }
inline bool evmDenseOnlyUnit() { return evmDenseOnlyUnitFlag(); }

/// Unit-global single-page decision (all layouts fit page 0). Only meaningful
/// when evmDenseOnlyUnit() is true.
inline bool& evmSinglePageUnitFlag()
{
	static bool singlePage = false;
	return singlePage;
}
inline void setEvmSinglePageUnit(bool _on) { evmSinglePageUnitFlag() = _on; }
inline bool evmSinglePageUnit() { return evmSinglePageUnitFlag(); }

} // namespace puyasol::builder
