// SPDX-License-Identifier: MIT
pragma solidity >=0.8.4;
// Isolate asm sdiv/smod edge cases with int256.min (-2^255).
contract AsmSdivMin {
    function tsdiv(int256 a, int256 b) external pure returns (int256 r) {
        assembly { r := sdiv(a, b) }
    }
    function tsmod(int256 a, int256 b) external pure returns (int256 r) {
        assembly { r := smod(a, b) }
    }
    // high-level signed div/mod for comparison (checked arithmetic — reverts on min/-1)
    function hdiv(int256 a, int256 b) external pure returns (int256) { return a / b; }
    function hmod(int256 a, int256 b) external pure returns (int256) { return a % b; }
}
