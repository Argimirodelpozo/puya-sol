// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract ArithEdges {
    function uncheckedAdd(uint256 a, uint256 b) external pure returns (uint256) { unchecked { return a + b; } }
    function uncheckedSub(uint256 a, uint256 b) external pure returns (uint256) { unchecked { return a - b; } }
    function uncheckedMul(uint256 a, uint256 b) external pure returns (uint256) { unchecked { return a * b; } }
    function uncheckedNeg(int256 a) external pure returns (int256) { unchecked { return -a; } }
    function expOp(uint256 b, uint256 e) external pure returns (uint256) { unchecked { return b ** e; } }
    function expChecked(uint128 b, uint8 e) external pure returns (uint256) { return uint256(b) ** e; }
    function shlOp(uint256 x, uint256 n) external pure returns (uint256) { unchecked { return x << n; } }
    function shrOp(uint256 x, uint256 n) external pure returns (uint256) { return x >> n; }
    function sarOp(int256 x, uint256 n) external pure returns (int256) { return x >> n; }
    function uncheckedInc(uint8 x) external pure returns (uint8) { unchecked { return x + 1; } }
    function signedMod(int256 a, int256 b) external pure returns (int256) { return a % b; }
    function signedDiv(int256 a, int256 b) external pure returns (int256) { return a / b; }
    function addmodOp(uint256 a, uint256 b, uint256 m) external pure returns (uint256) { return addmod(a,b,m); }
    function mulmodOp(uint256 a, uint256 b, uint256 m) external pure returns (uint256) { return mulmod(a,b,m); }
}
