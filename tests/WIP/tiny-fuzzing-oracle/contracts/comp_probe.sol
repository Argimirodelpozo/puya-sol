// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function f(uint64 a, uint64 b) external pure returns (uint64) { return (a << 7) & b; }    // shift on LEFT
    function g(uint16 a, uint16 b) external pure returns (uint16) { return b | (a >> 3); }      // shift on RIGHT
    function h(int16 a, int16 b)   external pure returns (int16)  { return (a >> 3) + b; }      // signed shift subexpr
    function k(uint8 a, uint8 b)   external pure returns (uint8)  { return ((a << 2) ^ b) & (b >> 1); }
}
