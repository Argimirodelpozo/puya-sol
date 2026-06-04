// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Guard for the biguint -> arc4.uintN (N<256) width trim (Node.h makeARC4Encode).
// A Yul `and(x, MASK)` yields a NON-minimal biguint: AVM `b&` keeps the WIDER
// operand's byte width, so a 32-byte value masked to 160 bits stays 32 bytes.
// Returning it as uintN must NOT trip puya's biguint->arc4.uintN `len <= n/8`
// overflow assert. The fix trims to the LOW n/8 bytes (= value mod 2^n) at the
// ABI encode. This is the unsigned-width sibling of the int24 sub-word ENCODE
// fix; it unblocked the V4 swap uint160 sqrtPrice math.
contract AsmUintNMask {
    function mask160(uint256 packed) external pure returns (uint160 r) {
        assembly {
            r := and(packed, 0xffffffffffffffffffffffffffffffffffffffff)
        }
    }

    function mask128(uint256 packed) external pure returns (uint128 r) {
        assembly {
            r := and(packed, 0xffffffffffffffffffffffffffffffff)
        }
    }
}
