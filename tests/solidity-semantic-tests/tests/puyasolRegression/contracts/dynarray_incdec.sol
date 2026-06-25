// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Companion to dynarray_compound_assign: arr[i]++ / arr[i]-- on a STORAGE dynamic-array element.
// Same ARC4-encoded-element decode issue (SolUnaryOperation::handleIncDec). The box_replace write
// persists; postfix returns OLD, prefix returns NEW; sub-word checked overflow reverts.
contract C {
    uint128[] a;
    int64[] b;
    uint8[] c;
    function pushA(uint128 v) external { a.push(v); }
    function postIncA(uint256 i) external returns (uint128) { return a[i]++; }   // returns OLD
    function preIncA(uint256 i) external returns (uint128) { return ++a[i]; }    // returns NEW
    function decA(uint256 i) external { a[i]--; }
    function getA(uint256 i) external view returns (uint128) { return a[i]; }
    function pushB(int64 v) external { b.push(v); }
    function incB(uint256 i) external { b[i]++; }
    function getB(uint256 i) external view returns (int64) { return b[i]; }
    function pushC(uint8 v) external { c.push(v); }
    function incC(uint256 i) external { c[i]++; }
    function getC(uint256 i) external view returns (uint8) { return c[i]; }
}
