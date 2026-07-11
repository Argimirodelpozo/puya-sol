// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A1 {
    function u128(uint256 a) external pure returns (uint128) { return uint128(a); }
    function u256(uint256 a) external pure returns (uint256) { return a; }
    function i64(int256 a) external pure returns (int64) { return int64(a); }
    function i128(int256 a) external pure returns (int128) { return int128(a); }
    function i256(int256 a) external pure returns (int256) { return a; }
    uint256 s;
    function setget(uint128 a) external returns (uint128) { s = a; return uint128(s); }  // named-ish via state
}
