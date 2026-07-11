// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract SxShift {
    function n1(int8 x)   external pure returns (int8) { return x >> 1; }
    function n7(int8 x)   external pure returns (int8) { return x >> 7; }
    function n8(int8 x)   external pure returns (int8) { return x >> 8; }
    function n100(int8 x) external pure returns (int8) { return x >> 100; }
    function n256(int8 x) external pure returns (int8) { return x >> 256; }
    function dynN(int8 x, uint256 s) external pure returns (int8) { return x >> s; }
}
