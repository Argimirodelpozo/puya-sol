// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    uint128[] a;
    int64[] b;
    uint8[] c;
    function pushA(uint128 v) external { a.push(v); }
    function postIncA(uint256 i) external returns (uint128) { if (i>=a.length) return 0; return a[i]++; }  // returns OLD
    function preIncA(uint256 i) external returns (uint128) { if (i>=a.length) return 0; return ++a[i]; }   // returns NEW
    function incThenReadA(uint256 i) external returns (uint128) { if (i>=a.length) return 0; a[i]++; return a[i]; }
    function decA(uint256 i) external { if (i<a.length) a[i]--; }
    function getA(uint256 i) external view returns (uint128) { return i<a.length?a[i]:0; }
    function pushB(int64 v) external { b.push(v); }
    function postIncB(uint256 i) external returns (int64) { if (i>=b.length) return 0; return b[i]++; }
    function getB(uint256 i) external view returns (int64) { return i<b.length?b[i]:int64(0); }
    function pushC(uint8 v) external { c.push(v); }
    function incC(uint256 i) external { if (i<c.length) c[i]++; }  // wraps/reverts at 255
    function getC(uint256 i) external view returns (uint8) { return i<c.length?c[i]:0; }
}
