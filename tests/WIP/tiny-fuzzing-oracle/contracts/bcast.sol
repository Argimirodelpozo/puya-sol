// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function b2u4(bytes4 a)  external pure returns (uint32)  { return uint32(a); }   // bytes -> uint
    function u2b4(uint32 a)  external pure returns (bytes4)  { return bytes4(a); }    // uint -> bytes
    function b2u32(bytes32 a) external pure returns (uint256){ return uint256(a); }
    function u2b32(uint256 a) external pure returns (bytes32){ return bytes32(a); }
    function u2b1(uint8 a)   external pure returns (bytes1)  { return bytes1(a); }
}
