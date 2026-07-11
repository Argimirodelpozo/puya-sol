// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function blen(bytes calldata b)  external pure returns (uint256) { return b.length; }
    function slen(string calldata s) external pure returns (uint256) { return bytes(s).length; }
    function bAt(bytes calldata b, uint256 i) external pure returns (bytes1) { return b[i]; }  // OOB reverts
    function id32(bytes32 x)         external pure returns (bytes32) { return x; }
    function id4(bytes4 x)           external pure returns (bytes4)  { return x; }
    function xor32(bytes32 a, bytes32 b) external pure returns (bytes32) { return a ^ b; }
    function and1(bytes1 a, bytes1 b)    external pure returns (bytes1)  { return a & b; }
    function notB(bytes1 a)          external pure returns (bytes1)  { return ~a; }
    function echo(bytes calldata b)  external pure returns (bytes memory) { return b; }
    function concat2(bytes calldata a, bytes calldata b) external pure returns (bytes memory) { return bytes.concat(a, b); }
}
