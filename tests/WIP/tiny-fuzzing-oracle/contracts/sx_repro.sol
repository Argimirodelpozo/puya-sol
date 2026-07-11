// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract SxRepro {
    function i8_rt(int256 x)   external pure returns (int256) { return int256(int8(x)); }           // int8 -> int256
    function i16_rt(int256 x)  external pure returns (int256) { return int256(int16(x)); }          // int16 -> int256
    function i8_to_i16(int256 x) external pure returns (int16) { return int16(int8(x)); }           // int8 -> int16
    function i8_i16_i256(int256 x) external pure returns (int256) { return int256(int16(int8(x))); }// chain
    function i8_local_widen(int256 x) external pure returns (int256) { int8 a = int8(x); int16 b = a; return b; } // via locals
}
