// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract Casts {
    function chain1(uint256 x) external pure returns (uint8) { return uint8(uint16(uint32(x))); }
    function chain2(int256 x) external pure returns (int8) { return int8(int16(int32(x))); }
    function u2i(uint256 x) external pure returns (int256) { return int256(x); }
    function i2u(int256 x) external pure returns (uint256) { return uint256(x); }
    function narrowSigned(int256 x) external pure returns (int128) { return int128(x); }
    function u8toi8(uint8 x) external pure returns (int8) { return int8(x); }
    function i8tou8(int8 x) external pure returns (uint8) { return uint8(x); }
    function crossWidth(uint64 x) external pure returns (int32) { return int32(uint32(x)); }
    function bytesToUint(bytes32 b) external pure returns (uint256) { return uint256(b); }
    function uintToBytes(uint256 x) external pure returns (bytes32) { return bytes32(x); }
    function addrToUint(address a) external pure returns (uint160) { return uint160(a); }
    function mixCast(int128 a, uint64 b) external pure returns (int256) { return int256(a) * int256(uint256(b)); }
}
