// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    function t0(int8 a, int8 c) external pure returns (int8) { unchecked { return a + c; } }
    function t1(int8 a, int8 c) external pure returns (int8) { unchecked { return (a + c) ** 3; } }
    function t3(int8 a) external pure returns (int8) { unchecked { return a ** 3; } }
    function t4(int8 a, int8 b) external pure returns (int8) { unchecked { return b ^ (a ** 3); } }
    function t5(uint8 a, uint8 b) external pure returns (uint8) { unchecked { return b ^ (a ** 3); } }
}
