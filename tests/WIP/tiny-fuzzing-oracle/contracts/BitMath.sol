// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// `Bits.bitlen` maps to the native AVM `bitlen` opcode (see WIP/tokens/AVM.sol
// and EVMfun.md). Replaces the EVM most/least-significant-bit bit-hackery below.
// `tokens/` resolves via puya-sol's built-in stdlib include path (the `WIP/`
// dir next to the executable), the same convention as `tokens/AERC20.sol`.
import {Bits} from "libs/AVM.sol";

/// @title BitMath
/// @dev This library provides functionality for computing bit properties of an unsigned integer
/// @author Solady (https://github.com/Vectorized/solady/blob/8200a70e8dc2a77ecb074fc2e99a2a0d36547522/src/utils/LibBit.sol)
library BitMath {
    /// @notice Returns the index of the most significant bit of the number,
    ///     where the least significant bit is at index 0 and the most significant bit is at index 255
    /// @param x the value for which to compute the most significant bit, must be greater than 0
    /// @return r the index of the most significant bit
    function mostSignificantBit(uint256 x) internal pure returns (uint8 r) {
        require(x > 0);

        // AVM: the most-significant-bit index is simply `bitlen(x) - 1`. The EVM
        // has no bitlen/CLZ opcode, so the original Uniswap code binary-searches
        // it with a de Bruijn byte table — which lowers to ~1000 lines of 256-bit
        // byte-array ops on the AVM. See EVMfun.md. Original EVM assembly kept
        // below for reference:
        //
        // assembly ("memory-safe") {
        //     r := shl(7, lt(0xffffffffffffffffffffffffffffffff, x))
        //     r := or(r, shl(6, lt(0xffffffffffffffff, shr(r, x))))
        //     r := or(r, shl(5, lt(0xffffffff, shr(r, x))))
        //     r := or(r, shl(4, lt(0xffff, shr(r, x))))
        //     r := or(r, shl(3, lt(0xff, shr(r, x))))
        //     r := or(r, byte(and(0x1f, shr(shr(r, x), 0x8421084210842108cc6318c6db6d54be)),
        //         0x0706060506020500060203020504000106050205030304010505030400000000))
        // }
        r = uint8(Bits.bitlen(x) - 1);
    }

    /// @notice Returns the index of the least significant bit of the number,
    ///     where the least significant bit is at index 0 and the most significant bit is at index 255
    /// @param x the value for which to compute the least significant bit, must be greater than 0
    /// @return r the index of the least significant bit
    function leastSignificantBit(uint256 x) internal pure returns (uint8 r) {
        require(x > 0);

        // AVM: isolate the lowest set bit (`x & -x`); its index is `bitlen-1`.
        // Original EVM de Bruijn implementation kept below for reference (see
        // EVMfun.md):
        //
        // assembly ("memory-safe") {
        //     x := and(x, sub(0, x))
        //     r := shl(5, shr(252, shl(shl(2, shr(250, mul(x,
        //         0xb6db6db6ddddddddd34d34d349249249210842108c6318c639ce739cffffffff))),
        //         0x8040405543005266443200005020610674053026020000107506200176117077)))
        //     r := or(r, byte(and(div(0xd76453e0, shr(r, x)), 0x1f),
        //         0x001f0d1e100c1d070f090b19131c1706010e11080a1a141802121b1503160405))
        // }
        uint256 lowest;
        unchecked {
            lowest = x & (0 - x); // two's-complement isolate of the lowest set bit
        }
        r = uint8(Bits.bitlen(lowest) - 1);
    }
}
