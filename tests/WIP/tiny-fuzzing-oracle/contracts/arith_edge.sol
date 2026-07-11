// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Classic edge cases: INT_MIN/-1 overflow, sub-word signed div/mod, exp overflow, addmod/mulmod, shifts.
contract ArithEdge {
    function sdiv(int256 a, int256 b) external pure returns (int256) { return a / b; }   // INT_MIN/-1 reverts
    function smod(int256 a, int256 b) external pure returns (int256) { return a % b; }
    function sdiv8(int8 a, int8 b)    external pure returns (int8)    { return a / b; }   // sub-word signed
    function smod8(int8 a, int8 b)    external pure returns (int8)    { return a % b; }
    function expU(uint256 b, uint256 e) external pure returns (uint256) { return b ** e; } // checked overflow
    function expU8(uint8 b, uint8 e)  external pure returns (uint8)   { return b ** e; }
    function sar(int256 x, uint256 n) external pure returns (int256)  { return x >> n; }  // arithmetic shift
    function sarI8(int8 x, uint256 n) external pure returns (int8)    { return x >> n; }  // sub-word arith shift
    function shlBig(uint256 x, uint256 n) external pure returns (uint256) { return x << n; }
    function negChecked(int256 x)     external pure returns (int256)  { return -x; }      // -INT_MIN reverts
}
