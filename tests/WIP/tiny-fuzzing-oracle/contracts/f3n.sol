// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function mulD(int256 d, int256 a) external pure returns (int256) { return (d ** 1) * a; }
    function addD(int256 d, int256 a) external pure returns (int256) { return (d ** 1) + a; }
    function orD(int256 d, int256 a)  external pure returns (int256) { return (d ** 1) | a; }
    function exp2(int256 d, int256 a) external pure returns (int256) { return (d ** 2) * a; }
    function mulLit(int256 d)         external pure returns (int256) { return (d ** 1) * 0; }
    function mulNoExp(int256 d, int256 a) external pure returns (int256) { return d * a; }
}
