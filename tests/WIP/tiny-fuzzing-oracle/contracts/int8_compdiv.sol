// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function compDiv(int8 a, int8 b) external pure returns (int8) { int8 x = a; unchecked { x /= b; } return x; }
    function plainDiv(int8 a, int8 b) external pure returns (int8) { unchecked { return a / b; } }
    function compMod(int8 a, int8 b) external pure returns (int8) { int8 x = a; unchecked { x %= b; } return x; }
    function compDiv16(int16 a, int16 b) external pure returns (int16) { int16 x = a; unchecked { x /= b; } return x; }
    function compDivU8(uint8 a, uint8 b) external pure returns (uint8) { uint8 x = a; unchecked { x /= b; } return x; }
}
