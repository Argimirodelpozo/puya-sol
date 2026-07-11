// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct P { int128 x; uint64 y; }
    P public p;                       // zero-arg struct getter — RESAMPLED after each mutation
    uint256[] public arr;             // arr(uint256) param getter + push
    function setX(int128 d) external { p.x += d; }       // signed compound on struct field
    function setY(uint64 v) external { p.y = v; }
    function push(uint256 v) external { arr.push(v); }
    function total() external view returns (uint256 s) { for (uint i=0;i<arr.length;i++) s += arr[i]; }
    function plen() external view returns (uint256) { return arr.length; }
}
