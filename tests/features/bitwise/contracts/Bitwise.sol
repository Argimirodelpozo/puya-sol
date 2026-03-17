// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Bitwise operations regression test.
contract Bitwise {
    function bitwiseAnd(uint256 a, uint256 b) external pure returns (uint256) { return a & b; }
    function bitwiseOr(uint256 a, uint256 b) external pure returns (uint256) { return a | b; }
    function bitwiseXor(uint256 a, uint256 b) external pure returns (uint256) { return a ^ b; }
    function bitwiseNot(uint256 a) external pure returns (uint256) { return ~a; }
    function shiftLeft(uint256 a, uint256 bits) external pure returns (uint256) { return a << bits; }
    function shiftRight(uint256 a, uint256 bits) external pure returns (uint256) { return a >> bits; }

    // Bytes32 bitwise
    function bytes32And(bytes32 a, bytes32 b) external pure returns (bytes32) { return a & b; }
    function bytes32Or(bytes32 a, bytes32 b) external pure returns (bytes32) { return a | b; }
    function bytes32Xor(bytes32 a, bytes32 b) external pure returns (bytes32) { return a ^ b; }
}
