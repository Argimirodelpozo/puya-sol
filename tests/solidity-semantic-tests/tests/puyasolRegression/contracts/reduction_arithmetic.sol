// SPDX-License-Identifier: MIT
pragma solidity ^0.8.26;

contract ReductionArithmetic {
    uint256 public calls;
    function divide64(int64 x, int64 y) external pure returns (int64) { return x / y; }
    function modulo64(int64 x, int64 y) external pure returns (int64) { return x % y; }
    function divide128(int128 x, int128 y) external pure returns (int128) { return x / y; }
    function modulo128(int128 x, int128 y) external pure returns (int128) { return x % y; }
    function divide256(int256 x, int256 y) external pure returns (int256) { return x / y; }
    function modulo256(int256 x, int256 y) external pure returns (int256) { return x % y; }
    function unchecked64(int64 x, int64 y) external pure returns (int64) { unchecked { return x / y; } }
    function unchecked128(int128 x, int128 y) external pure returns (int128) { unchecked { return x / y; } }
    function unchecked256(int256 x, int256 y) external pure returns (int256) { unchecked { return x / y; } }
    function yul(uint256 x, uint256 y) external pure returns (uint256 q, uint256 r) {
        assembly { q := sdiv(x, y) r := smod(x, y) }
    }
    function operand(int64 x) internal returns (int64) { ++calls; return x; }
    function effects(int64 x, int64 y) external returns (int64, uint256) {
        return (operand(x) / operand(y), calls);
    }
}

contract ReductionDivideOnly {
    function divide(int64 x, int64 y) external pure returns (int64) { return x / y; }
}
