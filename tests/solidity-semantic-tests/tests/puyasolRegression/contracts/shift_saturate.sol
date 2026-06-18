// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test.
// EVM/Solidity `<<` and `>>` saturate to 0 when the shift amount is >= 256
// (shifts truncate; they are never overflow-checked). The high-level uint256
// shift path (buildBigUIntShift) used to REVERT for shift >= 256 because the
// setbit-based 2^shift indexed bit `255 - shift`, which underflowed in uint64
// and panicked on the AVM. Now guarded like the assembly shl/shr handlers and
// the signed-SAR path: clamp the index via `mod 256` + `(shift < 256) ? v : 0`.
// Surfaced by tests/WIP/tiny-fuzzing-oracle (differential-fuzzing spike).
contract ShiftSaturate {
    function shl(uint256 x, uint256 s) external pure returns (uint256) { return x << s; }
    function shr(uint256 x, uint256 s) external pure returns (uint256) { return x >> s; }
}
