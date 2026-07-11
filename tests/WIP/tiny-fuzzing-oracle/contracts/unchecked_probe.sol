// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// unchecked wrap-around vs checked revert, across widths + signed.
contract Unchecked {
    function addW8(uint8 a, uint8 b)    external pure returns (uint8)   { unchecked { return a + b; } }
    function subW8(uint8 a, uint8 b)    external pure returns (uint8)   { unchecked { return a - b; } }
    function mulW8(uint8 a, uint8 b)    external pure returns (uint8)   { unchecked { return a * b; } }
    function addW256(uint256 a, uint256 b) external pure returns (uint256) { unchecked { return a + b; } }
    function subWi8(int8 a, int8 b)     external pure returns (int8)    { unchecked { return a - b; } }
    function mulWi8(int8 a, int8 b)     external pure returns (int8)    { unchecked { return a * b; } }
    function negWi(int256 a)            external pure returns (int256)  { unchecked { return -a; } }
    function incW8(uint8 a)             external pure returns (uint8)   { unchecked { a++; return a; } }
    function addChecked8(uint8 a, uint8 b) external pure returns (uint8) { return a + b; }  // reverts on overflow
}
