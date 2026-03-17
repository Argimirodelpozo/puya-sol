// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Comparison operators regression test.
contract Comparison {
    function eq(uint256 a, uint256 b) external pure returns (bool) { return a == b; }
    function ne(uint256 a, uint256 b) external pure returns (bool) { return a != b; }
    function lt(uint256 a, uint256 b) external pure returns (bool) { return a < b; }
    function lte(uint256 a, uint256 b) external pure returns (bool) { return a <= b; }
    function gt(uint256 a, uint256 b) external pure returns (bool) { return a > b; }
    function gte(uint256 a, uint256 b) external pure returns (bool) { return a >= b; }

    function eqAddr(address a, address b) external pure returns (bool) { return a == b; }
    function eqBool(bool a, bool b) external pure returns (bool) { return a == b; }
    function eqBytes32(bytes32 a, bytes32 b) external pure returns (bool) { return a == b; }
}
