// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function b32ToU(bytes32 x) external pure returns (uint256) { return uint256(x); }     // big-endian
    function uToB32(uint256 x) external pure returns (bytes32) { return bytes32(x); }
    function b32ToB4(bytes32 x) external pure returns (bytes4) { return bytes4(x); }        // HIGH 4 bytes
    function b4ToB32(bytes4 x)  external pure returns (bytes32) { return bytes32(x); }       // left-aligned
    function b1ToU8(bytes1 x)   external pure returns (uint8)  { return uint8(x); }
    function u8ToB1(uint8 x)    external pure returns (bytes1) { return bytes1(x); }
    function packed(bytes calldata a, uint8 b) external pure returns (bytes memory) { return abi.encodePacked(a, b); }
    function packedInts(uint16 a, uint8 b)     external pure returns (bytes memory) { return abi.encodePacked(a, b); }
    function sliceMid(bytes calldata b) external pure returns (bytes memory) { return b.length >= 4 ? b[1:3] : b; }
    function arrBytesLen(bytes[] calldata bs) external pure returns (uint256 s) { for (uint i; i < bs.length; i++) s += bs[i].length; }
    function sha(bytes calldata b) external pure returns (bytes32) { return sha256(b); }
}
