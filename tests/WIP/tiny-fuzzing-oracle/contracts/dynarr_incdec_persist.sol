// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint128[] a;
    int64[] b;
    function pushA(uint128 v) external { a.push(v); }
    function incA(uint256 i) external { a[i]++; }
    function decA(uint256 i) external { a[i]--; }
    function getA(uint256 i) external view returns (uint128) { return a[i]; }
    function pushB(int64 v) external { b.push(v); }
    function incB(uint256 i) external { b[i]++; }
    function getB(uint256 i) external view returns (int64) { return b[i]; }
}
