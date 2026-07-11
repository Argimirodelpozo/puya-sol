// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function f25(int256 a, int256 b, int256 c, int256 d) external pure returns (int256) { return ((d ** 1) * a); }
    function only2(int256 a, int256 d) external pure returns (int256) { return ((d ** 1) * a); }
}
