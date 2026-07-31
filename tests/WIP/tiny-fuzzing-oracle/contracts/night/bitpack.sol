// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract BitPack {
    function pack(uint64 a, uint64 b, uint64 c, uint64 d) external pure returns (uint256) {
        return (uint256(a) << 192) | (uint256(b) << 128) | (uint256(c) << 64) | uint256(d);
    }
    function unpackA(uint256 p) external pure returns (uint64) { return uint64(p >> 192); }
    function unpackB(uint256 p) external pure returns (uint64) { return uint64(p >> 128); }
    function unpackC(uint256 p) external pure returns (uint64) { return uint64(p >> 64); }
    function unpackD(uint256 p) external pure returns (uint64) { return uint64(p); }
    function setByte(uint256 word, uint256 idx, uint8 v) external pure returns (uint256) {
        uint256 shift = (31 - (idx % 32)) * 8;
        return (word & ~(uint256(0xff) << shift)) | (uint256(v) << shift);
    }
    function getByte(uint256 word, uint256 idx) external pure returns (uint8) {
        return uint8(word >> ((31 - (idx % 32)) * 8));
    }
    function masks(uint256 x, uint8 bits) external pure returns (uint256) {
        bits = uint8(bits % 256);
        uint256 mask = bits == 0 ? 0 : (bits >= 256 ? type(uint256).max : (uint256(1) << bits) - 1);
        return x & mask;
    }
}
