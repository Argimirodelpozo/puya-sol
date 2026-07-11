// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function combo(int256 d, int256 a) external pure returns (int256) { return (d ** 1) * a; }   // (d,a) order
    function f25(int256 a, int256 d)   external pure returns (int256) { return (d ** 1) * a; }   // (a,d) order — the finding
    function mul(int256 a, int256 b)   external pure returns (int256) { return a * b; }           // INT_MIN * 0
    function exp1(int256 d)            external pure returns (int256) { return d ** 1; }
    function mulExp(int256 d, int256 a) external pure returns (int256) { int256 x = d ** 1; return x * a; }  // materialised
}
