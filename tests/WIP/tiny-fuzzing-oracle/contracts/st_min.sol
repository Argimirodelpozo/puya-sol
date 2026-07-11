// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { uint8 u8; int16 i16; uint64 u64; int128 i128; }
    S public s;                              // auto-getter
    function addI128(int128 d) external { s.i128 += d; }    // compound on the 4th (packed) field
}
