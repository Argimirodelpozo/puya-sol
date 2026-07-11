// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    mapping(uint256 => int8) mi8;
    mapping(uint256 => uint128) mu128;
    mapping(uint256 => mapping(uint256 => int16)) nested;
    struct S { int8 a; uint128 b; int64 c; }
    S st;
    // mapping signed sub-word compound (checked overflow)
    function setMi8(uint256 k, int8 v) external { mi8[k] = v; }
    function addMi8(uint256 k, int8 v) external returns (int8) { mi8[k] += v; return mi8[k]; }
    function divMi8(uint256 k, int8 v) external returns (int8) { mi8[k] /= v; return mi8[k]; }   // int8.min/-1
    // mapping wide unsigned compound
    function setMu128(uint256 k, uint128 v) external { mu128[k] = v; }
    function mulMu128(uint256 k, uint128 v) external returns (uint128) { mu128[k] *= v; return mu128[k]; }
    // nested mapping signed compound
    function setNested(uint256 a, uint256 b, int16 v) external { nested[a][b] = v; }
    function addNested(uint256 a, uint256 b, int16 v) external returns (int16) { nested[a][b] += v; return nested[a][b]; }
    // struct signed sub-word field compound
    function setSt(int8 a, uint128 b, int64 c) external { st = S(a,b,c); }
    function addStA(int8 v) external returns (int8) { st.a += v; return st.a; }
    function divStC(int64 v) external returns (int64) { st.c /= v; return st.c; }
    // ++ on mapping/struct sub-word
    function incMi8(uint256 k) external returns (int8) { mi8[k]++; return mi8[k]; }
    function incStA() external returns (int8) { st.a++; return st.a; }
}
