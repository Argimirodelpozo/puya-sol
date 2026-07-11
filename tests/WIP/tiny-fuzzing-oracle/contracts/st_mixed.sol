// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract MX {
    struct S { uint8 u8; int16 i16; uint64 u64; int128 i128; }
    S public s;
    mapping(uint256 => S) public m;
    function setU8(uint8 v)            external { s.u8 = v; }
    function setI16(int16 v)           external { s.i16 = v; }
    function incU64(uint64 d)          external { s.u64 += d; }
    function addI128(int128 d)         external { s.i128 += d; }    // signed compound on packed struct field
    function setMU8(uint256 k, uint8 v) external { m[k].u8 = v; }
    function addMI128(uint256 k, int128 d) external { m[k].i128 += d; }
}
