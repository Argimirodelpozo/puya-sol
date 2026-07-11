// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { uint8 u8; int16 i16; uint64 u64; int128 i128; }
    S public s;                                 // auto-getter — i128 is the 4th (packed) field
    function setI128(int128 v) external { s.i128 = v; }   // DIRECT set (no compound)
    function rt() external view returns (int128) { return s.i128; }   // explicit getter control
}
