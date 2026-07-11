// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function f(int16 a, uint64 b) external pure returns (int16, uint64) { return (a, b); }   // explicit tuple
    struct S { uint8 u8; int16 i16; uint64 u64; }
    S public s;                                                                                // auto-getter
    function setI16(int16 v) external { s.i16 = v; }
}
