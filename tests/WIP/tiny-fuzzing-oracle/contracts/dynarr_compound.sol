// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint128[] a128;
    int64[] aI64;
    uint256[] a256;
    uint8[] a8;
    function push128(uint128 v) external { a128.push(v); }
    function add128(uint256 i, uint128 b) external returns (uint128) { a128[i] += b; return a128[i]; }
    function mul128(uint256 i, uint128 b) external returns (uint128) { unchecked { a128[i] *= b; } return a128[i]; }
    function pushI64(int64 v) external { aI64.push(v); }
    function addI64(uint256 i, int64 b) external returns (int64) { aI64[i] += b; return aI64[i]; }
    function subI64(uint256 i, int64 b) external returns (int64) { aI64[i] -= b; return aI64[i]; }
    function push256(uint256 v) external { a256.push(v); }
    function or256(uint256 i, uint256 b) external returns (uint256) { a256[i] |= b; return a256[i]; }
    function div256(uint256 i, uint256 b) external returns (uint256) { a256[i] /= b; return a256[i]; }
    function push8(uint8 v) external { a8.push(v); }
    function add8(uint256 i, uint8 b) external returns (uint8) { a8[i] += b; return a8[i]; }
}
