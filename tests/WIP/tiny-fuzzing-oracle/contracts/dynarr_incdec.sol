// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint128[] a128;
    int64[] aI64;
    uint8[] a8;
    uint256[] a256;
    function push128(uint128 v) external { a128.push(v); }
    function inc128(uint256 i) external returns (uint128) { a128[i]++; return a128[i]; }
    function dec128(uint256 i) external returns (uint128) { a128[i]--; return a128[i]; }
    function preInc128(uint256 i) external returns (uint128) { return ++a128[i]; }
    function pushI64(int64 v) external { aI64.push(v); }
    function incI64(uint256 i) external returns (int64) { aI64[i]++; return aI64[i]; }
    function postRetI64(uint256 i) external returns (int64) { return aI64[i]++; }   // postfix returns OLD
    function push8(uint8 v) external { a8.push(v); }
    function inc8(uint256 i) external returns (uint8) { a8[i]++; return a8[i]; }
    function push256(uint256 v) external { a256.push(v); }
    function inc256(uint256 i) external returns (uint256) { a256[i]++; return a256[i]; }
}
