// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function addI128(int128 a, int128 b) external pure returns (int128) { return a + b; }
    function subI128(int128 a, int128 b) external pure returns (int128) { return a - b; }
    function mulI128(int128 a, int128 b) external pure returns (int128) { return a * b; }
    function addI256(int256 a, int256 b) external pure returns (int256) { return a + b; }
    function mulI256(int256 a, int256 b) external pure returns (int256) { return a * b; }
    function addI8(int8 a, int8 b)       external pure returns (int8)   { return a + b; }
}
