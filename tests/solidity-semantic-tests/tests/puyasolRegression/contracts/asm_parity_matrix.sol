// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Inline-assembly (Yul) op semantics matrix vs the EVM. Cells chosen for
// EVM-quirk risk: zero divisors return 0 (never panic), sdiv(min,-1) wraps,
// smod takes the dividend's sign, exp wraps mod 2^256 with 0**0=1,
// byte/signextend ignore out-of-range indices, shifts >=256 saturate,
// calldataload/mload past the written end read zeros.
contract AsmParity {
    function divmodZero() public pure returns (uint256 d, uint256 m, uint256 sd, uint256 sm) {
        assembly {
            d := div(7, 0)      // 0
            m := mod(7, 0)      // 0
            sd := sdiv(7, 0)    // 0
            sm := smod(7, 0)    // 0
        }
    }

    function sdivMinNegOne() public pure returns (uint256 r) {
        assembly {
            let min := shl(255, 1) // int256.min
            r := sdiv(min, not(0)) // EVM: wraps back to min, no panic
        }
    }

    function smodSigns() public pure returns (uint256 a, uint256 b) {
        assembly {
            // smod(-7, 3) = -1 (dividend's sign); smod(7, -3) = 1
            a := smod(sub(0, 7), 3)
            b := smod(7, sub(0, 3))
        }
    }

    function sdivRounding() public pure returns (uint256 a, uint256 b) {
        assembly {
            // sdiv truncates toward zero: -7/2 = -3; 7/-2 = -3
            a := sdiv(sub(0, 7), 2)
            b := sdiv(7, sub(0, 2))
        }
    }

    function expCells(uint256 x, uint256 y) public pure returns (uint256 r) {
        assembly {
            r := exp(x, y) // runtime: wraps mod 2^256; 0**0 = 1
        }
    }

    function byteCells() public pure returns (uint256 a, uint256 b, uint256 c) {
        assembly {
            let w := 0x1122334455667788000000000000000000000000000000000000000000000099
            a := byte(0, w)  // 0x11
            b := byte(31, w) // 0x99
            c := byte(32, w) // 0 (out of range)
        }
    }

    function signextendCells() public pure returns (uint256 a, uint256 b, uint256 c) {
        assembly {
            a := signextend(0, 0x80)   // -128 as uint256
            b := signextend(0, 0x7F)   // 0x7F
            c := signextend(31, 0x80)  // i>=31: value unchanged
        }
    }

    function signedCompareBoundary() public pure returns (uint256 a, uint256 b, uint256 c) {
        assembly {
            let min := shl(255, 1)
            a := slt(min, 0)      // 1 (int256.min < 0)
            b := sgt(0, min)      // 1
            c := slt(sub(min, 1), min) // slt(int256.max, min) = 0
        }
    }

    function shiftSaturation() public pure returns (uint256 a, uint256 b, uint256 c, uint256 d) {
        assembly {
            a := shl(256, 1)          // 0
            b := shr(256, not(0))     // 0
            c := sar(256, not(0))     // all ones (negative saturates)
            d := sar(256, 5)          // 0 (positive saturates)
        }
    }

    function calldataPastEnd() public pure returns (uint256 r) {
        assembly {
            // reads beyond calldatasize zero-pad
            r := calldataload(calldatasize())
        }
    }

    function mloadFreshZero() public pure returns (uint256 r) {
        assembly {
            r := mload(0x2000) // never-written memory reads zero
        }
    }

    function addmodMulmodHuge() public pure returns (uint256 a, uint256 b, uint256 c) {
        assembly {
            let big := not(0)
            a := addmod(big, big, 7)  // (2*(2^256-1)) % 7 intermediate NOT truncated
            b := mulmod(big, big, 7)
            c := addmod(5, 5, 0)      // modulus 0 -> 0
        }
    }

    function notIszero() public pure returns (uint256 a, uint256 b, uint256 c) {
        assembly {
            a := not(0xFF)     // 2^256-256
            b := iszero(0)     // 1
            c := iszero(2)     // 0
        }
    }
}
