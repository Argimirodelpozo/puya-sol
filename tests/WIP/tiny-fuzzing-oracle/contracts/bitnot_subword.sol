// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function f0(uint8 a, uint8 b, uint8 c, uint8 d) external pure returns (uint8) {
        return ((((b & b) & a) ^ (b + (~c))) + c);
    }
    function justNot(uint8 c) external pure returns (uint8) { return ~c; }
    function notU16(uint16 c) external pure returns (uint16) { return ~c; }
    function notAddU(uint8 b, uint8 c) external pure returns (uint8) { unchecked { return b + (~c); } }
    function notU128(uint128 c) external pure returns (uint128) { return ~c; }
    function xorNot(uint8 a, uint8 c) external pure returns (uint8) { return a ^ (~c); }
}
