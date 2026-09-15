#pragma once

/// @file EvmLayoutMode.h
/// Constants for the EVM-compatible storage backend. Per-build mode choices
/// live in TargetProfile; this header deliberately carries no mutable state.

namespace puyasol::builder
{

/// Slots below this are the DENSE region (sequential declared vars → paged
/// boxes, 64 slots per 2048-byte page = one box-ref budget). Keccak-derived
/// slots are astronomically larger, so a single runtime comparison splits the
/// regimes. 2^16 is generous: no real contract declares 65,536 top-level slots.
inline constexpr unsigned long long kEvmDenseSlotLimit = 1ULL << 16;
/// Slots per page box (64 x 32 B = 2048 B, exactly one box-reference budget).
inline constexpr unsigned kEvmSlotsPerPage = 64;

} // namespace puyasol::builder
