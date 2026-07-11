// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Signed div/mod sign rules + INT_MIN/-1 overflow + sub-word.
contract DivMod {
    function sdiv8(int8 a, int8 b)   external pure returns (int8)   { return a / b; }
    function smod8(int8 a, int8 b)   external pure returns (int8)   { return a % b; }
    function sdiv256(int256 a, int256 b) external pure returns (int256) { return a / b; }   // INT_MIN/-1 reverts
    function smod256(int256 a, int256 b) external pure returns (int256) { return a % b; }
    function udiv8(uint8 a, uint8 b) external pure returns (uint8)  { return a / b; }
    function expI(int8 b, uint8 e)   external pure returns (int8)   { return b ** e; }       // signed base exp
}
