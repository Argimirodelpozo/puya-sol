// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    uint128[] a;
    int64[] b;
    uint256[] c;
    function pushA(uint128 v) external { a.push(v); }
    function addA(uint256 i, uint128 v) external { if (i < a.length) { unchecked { a[i] += v; } } }
    function mulA(uint256 i, uint128 v) external { if (i < a.length) { unchecked { a[i] *= v; } } }
    function getA(uint256 i) external view returns (uint128) { return i < a.length ? a[i] : 0; }
    function pushB(int64 v) external { b.push(v); }
    function subB(uint256 i, int64 v) external { if (i < b.length) { unchecked { b[i] -= v; } } }
    function getB(uint256 i) external view returns (int64) { return i < b.length ? b[i] : int64(0); }
    function pushC(uint256 v) external { c.push(v); }
    function orC(uint256 i, uint256 v) external { if (i < c.length) { c[i] |= v; } }
    function getC(uint256 i) external view returns (uint256) { return i < c.length ? c[i] : 0; }
}
