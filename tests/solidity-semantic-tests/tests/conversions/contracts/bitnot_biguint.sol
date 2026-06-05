// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// NOTE: puya-sol CUSTOM regression test — NOT part of the upstream Solidity
// semantic-tests suite (added to guard a puya-sol-specific codegen path).

// Guard: bitwise NOT (~) on a biguint-backed intN (N>64) must truncate to the
// type's bit width. A raw byte-level invert produced the wrong number of bits
// (~uint128(5) -> 0xFA instead of 2^128-6), so the V4 LPFee pattern
// `x & ~OVERRIDE_FEE_FLAG` silently became a no-op / wrong mask.
contract BitNotBiguint {
    function notU128(uint128 x) external pure returns (uint128) {
        return ~x;
    }

    // The LPFee removeOverrideFlagAndValidate shape on a 128-bit flag: clearing a
    // single high bit via `& ~FLAG` must leave the other bits intact.
    function clearTopBit(uint128 x) external pure returns (uint128) {
        uint128 FLAG = uint128(1) << 127;
        return x & ~FLAG;
    }

    function notU160(uint160 x) external pure returns (uint160) {
        return ~x;
    }
}
