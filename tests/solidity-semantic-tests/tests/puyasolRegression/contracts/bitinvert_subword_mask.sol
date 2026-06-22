// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--cast).
// Bitwise NOT of a sub-256 biguint type (uint65..uint248) inverted the full 32-byte word, so
// `~uint128(0)` produced 2^256-1 instead of the mod-2^N value 2^128-1. Consumers that re-mask
// (store/return/`& y`/compare) hid it, but a downstream CHECKED add overflow-checks the un-masked value:
// `(~b) + a` tested `2^256-1 <= 2^128-1` and false-reverted; `(~c) / max` returned ~2^128 not 1.
// FIX: mask the biguint `~x` result back to 2^bits for m_bits < 256 (uint256 keeps the full-width invert).
contract BitinvertSubwordMask {
    function invAddU128(uint128 b, uint128 a) external pure returns (uint128) { return (~b) + a; }
    function invDivU128(uint128 c)            external pure returns (uint128) { return (~c) / type(uint128).max; }
    function invMaskU128(uint128 c)           external pure returns (bool)    { return (~c) <= type(uint128).max; }
    function invAddU192(uint192 b)            external pure returns (uint192) { return (~b) + 1; }
    function invU256(uint256 c)               external pure returns (uint256) { return ~c; } // full-width, unaffected
}
