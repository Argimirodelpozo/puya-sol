// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function t3(int8 a) external pure returns (int8) { unchecked { return a ** 3; } }
    function t3c(int8 a) external pure returns (int8) { return a ** 3; }   // checked
}
