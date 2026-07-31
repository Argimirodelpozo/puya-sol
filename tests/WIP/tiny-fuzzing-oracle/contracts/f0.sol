// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract F0 {
    // exact f0 from seed 700001
    function f0(uint16 a, uint16 b, uint16 c, uint16 d) external pure returns (uint16) {
        unchecked { return (d - (uint16(uint128(c)))); }
    }
    // simplified: does the cast chain matter?
    function sub(uint16 c, uint16 d) external pure returns (uint16) { unchecked { return d - c; } }
    function castOnly(uint16 c) external pure returns (uint16) { return uint16(uint128(c)); }
}
