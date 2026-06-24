// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the asm-opcode fuzz probe.
// Yul `sar(0, x)` (arithmetic shift-right by ZERO) returned all-ones (-1) for a negative x instead
// of x unchanged. Root: complementShift = 256 - shift = 256 at shift 0, and 2^256 overflows u256
// (wraps to 1), so fillMask = MAX -> shr|MAX = -1. The shift>=256 boundary was handled but not the
// shift==0 boundary. FIX: short-circuit sar(0, x) = x. Other shift amounts unchanged.
contract C {
    function sar0(uint256 x) external pure returns (uint256 r) { assembly { r := sar(0, x) } }
    function sarN(uint256 n, uint256 x) external pure returns (uint256 r) { assembly { r := sar(n, x) } }
}
