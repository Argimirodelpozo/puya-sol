// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (inline assembly Yul).
// Yul `byte(n, x)` returns byte n (big-endian, 0 = most significant) of the 32-byte x; for n >= 32 EVM
// returns 0 (out of range). The AVM lowering did `extract3(pad32(x), n, 1)`, which REVERTS for n >= 32
// (offset past the 32-byte value). Fix guards with `n < 32 ? byte : 0` (the conditional only evaluates
// the extract on the taken branch) — same shape as the shift>=256 saturate fix. In-range unchanged.
contract YulByteOutOfRange {
    function byteF(uint256 i, uint256 x) external pure returns (uint256 r) { assembly { r := byte(i, x) } }
}
