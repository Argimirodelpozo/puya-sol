// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function cadd(uint128 a, uint128 b) external pure returns (uint128) { uint128 x=a; x += b; return x; }
    function cmul(uint128 a, uint128 b) external pure returns (uint128) { uint128 x=a; x *= b; return x; }
    function csub(uint64 a, uint64 b) external pure returns (uint64) { uint64 x=a; x -= b; return x; }
    function csadd(int128 a, int128 b) external pure returns (int128) { int128 x=a; x += b; return x; }
    function csmul(int64 a, int64 b) external pure returns (int64) { int64 x=a; x *= b; return x; }
}
