// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint128[] a;
    int64[] b;
    function pushA(uint128 v) external { a.push(v); }
    function addA(uint256 i, uint128 v) external { a[i] += v; }
    function getA(uint256 i) external view returns (uint128) { return a[i]; }
    function pushB(int64 v) external { b.push(v); }
    function subB(uint256 i, int64 v) external { b[i] -= v; }
    function getB(uint256 i) external view returns (int64) { return b[i]; }
    function lenA() external view returns (uint256) { return a.length; }
}
