// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function shl4(bytes4 a, uint8 k)  external pure returns (bytes4)  { return a << k; }
    function shr4(bytes4 a, uint8 k)  external pure returns (bytes4)  { return a >> k; }
    function shl1(bytes1 a, uint8 k)  external pure returns (bytes1)  { return a << k; }
    function shl32(bytes32 a, uint8 k) external pure returns (bytes32) { return a << k; }
    function shr32(bytes32 a, uint8 k) external pure returns (bytes32) { return a >> k; }
    function shl16(bytes16 a, uint16 k) external pure returns (bytes16){ return a << k; }
    function comp(bytes4 a, bytes4 b)  external pure returns (bytes4)  { return (a << 8) | b; }
}
