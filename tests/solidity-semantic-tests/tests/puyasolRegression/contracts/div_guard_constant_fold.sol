// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// A div/mod whose divisor is a compile-time non-zero constant needs no
// zero-divisor guard. The guard's ternary costs three basic blocks each, and
// poseidon chains hundreds of them through one field prime.
contract DivGuardFold {
    function mulmodConst(uint256 a, uint256 b) external pure returns (uint256 r) {
        assembly {
            let F := 21888242871839275222246405745257275088548364400416034343698204186575808495617
            r := mulmod(a, b, F)
        }
    }

    function addmodConst(uint256 a, uint256 b) external pure returns (uint256 r) {
        assembly {
            let F := 21888242871839275222246405745257275088548364400416034343698204186575808495617
            r := addmod(a, b, F)
        }
    }

    function modSmallConst(uint256 a) external pure returns (uint256 r) {
        assembly {
            let m := 97
            r := mod(a, m)
        }
    }

    // Guard must SURVIVE below: EVM returns 0 for a zero divisor.
    function divByLiteralZero(uint256 a) external pure returns (uint256 r) {
        assembly { r := div(a, 0) }
    }

    function divByZeroLocal(uint256 a) external pure returns (uint256 r) {
        assembly {
            let d := 0
            r := div(a, d)
        }
    }

    function divByRuntime(uint256 a, uint256 d) external pure returns (uint256 r) {
        assembly { r := div(a, d) }
    }

    // Reassigned local: the fold is flow-insensitive, so it must not apply.
    function modByReassigned(uint256 a, bool zero) external pure returns (uint256 r) {
        assembly {
            let m := 97
            if zero { m := 0 }
            r := mod(a, m)
        }
    }
}
