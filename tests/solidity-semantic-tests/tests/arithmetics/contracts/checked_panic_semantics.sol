// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    int256 constant MIN = type(int256).min;
    function minDiv() external pure returns (int256) { int256 m = MIN; int256 n = -1; return m / n; }       // panic 0x11
    function negMin() external pure returns (int256) { int256 m = MIN; return -m; }                          // panic 0x11
    function minSub1() external pure returns (int256) { int256 m = MIN; int256 o = 1; return m - o; }        // panic 0x11
    function i8Over() external pure returns (int8)   { int8 x = 127; int8 o = 1; return x + o; }             // panic 0x11
    function expOver() external pure returns (int256){ int256 b = 2; uint256 e = 256; return b ** e; }       // panic 0x11
    function uUnder() external pure returns (uint256){ uint256 z = 0; uint256 o = 1; return z - o; }         // panic 0x11
    function divZero() external pure returns (int256){ int256 a = 5; int256 z = 0; return a / z; }           // panic 0x12
    function modZero() external pure returns (uint256){ uint256 a = 5; uint256 z = 0; return a % z; }        // panic 0x12
}
