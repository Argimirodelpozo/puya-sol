// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--cast).
// `unchecked` uint64 multiplication (and addition) that OVERFLOWS 2^64 reverted on the AVM (the `*`/`+`
// opcodes panic on overflow) where Solidity wraps mod 2^64. The sub-word (<64-bit) path masks to 2^N and
// Sub/Pow at uint64 were already routed through wrapping paths, but the full-width uint64 Add/Mult fell
// through to the panicking opcode (the "Add/Mult already wrap" comment was wrong for uint64). FIX:
// uint64 unchecked Add/Mult wide-compute via biguint, mod 2^64, narrow back. Checked mul still reverts.
contract UncheckedUint64MulAdd {
    function mul(uint64 a, uint64 b) external pure returns (uint64) { unchecked { return a * b; } }
    function add(uint64 a, uint64 b) external pure returns (uint64) { unchecked { return a + b; } }
    function mulChecked(uint64 a, uint64 b) external pure returns (uint64) { return a * b; }   // reverts on overflow
    function addChecked(uint64 a, uint64 b) external pure returns (uint64) { return a + b; }   // reverts on overflow
}
