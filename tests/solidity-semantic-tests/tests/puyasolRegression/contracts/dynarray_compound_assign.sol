// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Found by the differential fuzzer: compound assignment on a STORAGE dynamic-array element
// (arr[i] += / -= / *= / |= / /= b) failed to compile (puya backend itob(Encoded(uintN))) because the
// compound read reused the ARC4-encoded write-form element without decoding.
contract C {
    uint128[] a;
    int64[] b;
    uint256[] c;
    uint8[] d;
    function pushA(uint128 v) external { a.push(v); }
    function addA(uint256 i, uint128 v) external { a[i] += v; }
    function getA(uint256 i) external view returns (uint128) { return a[i]; }
    function pushB(int64 v) external { b.push(v); }
    function subB(uint256 i, int64 v) external { b[i] -= v; }
    function getB(uint256 i) external view returns (int64) { return b[i]; }
    function pushC(uint256 v) external { c.push(v); }
    function divC(uint256 i, uint256 v) external { c[i] /= v; }
    function getC(uint256 i) external view returns (uint256) { return c[i]; }
    function pushD(uint8 v) external { d.push(v); }
    function addD(uint256 i, uint8 v) external { d[i] += v; }
    function getD(uint256 i) external view returns (uint8) { return d[i]; }
}
