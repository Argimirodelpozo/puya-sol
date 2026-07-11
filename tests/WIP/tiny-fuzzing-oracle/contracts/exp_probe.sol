// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function combo(int256 d, int256 a) external pure returns (int256) { return (d ** 1) * a; }   // the seed-2019 shape
    function exp1(int256 d) external pure returns (int256) { return d ** 1; }
    function mul(int256 a, int256 b) external pure returns (int256) { return a * b; }              // INT_MIN * 0 ?
    function expU1(uint256 d) external pure returns (uint256) { return d ** 1; }
}
