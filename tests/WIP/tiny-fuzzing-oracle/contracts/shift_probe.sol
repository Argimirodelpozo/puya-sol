// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function shl16_256(uint16 a)  external pure returns (uint16)  { return a << 256; }
    function shl16_64(uint16 a)   external pure returns (uint16)  { return a << 64; }
    function shl16_16(uint16 a)   external pure returns (uint16)  { return a << 16; }   // shift == width
    function shr16_256(uint16 a)  external pure returns (uint16)  { return a >> 256; }
    function shl128_256(uint128 a) external pure returns (uint128) { return a << 256; }
    function shlI16_256(int16 a)  external pure returns (int16)   { return a << 256; }
    function shlVar(uint16 a, uint256 s) external pure returns (uint16) { return a << s; }  // fuzz the shift amount
    function shl64_64(uint64 a)   external pure returns (uint64)  { return a << 64; }
}
