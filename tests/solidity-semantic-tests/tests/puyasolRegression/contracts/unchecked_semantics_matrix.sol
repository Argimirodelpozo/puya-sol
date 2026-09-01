// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract UncheckedProbe {
    function u1() public pure returns (uint8) {
        uint8 x = 255;
        unchecked { x = x + 1; }        // wraps to 0
        return x;
    }
    function u2() public pure returns (uint256) {
        uint256 x = 0;
        unchecked { x--; }              // wraps to 2^256-1
        return x;
    }
    function u3(uint256 a, uint256 b) public pure returns (uint256) {
        unchecked { return a / b; }     // division by zero STILL panics (0x12)
    }
    function u4() public pure returns (int256) {
        int256 x = type(int256).min;
        unchecked { x = -x; }           // -min wraps to min
        return x;
    }
    function helperAdd(uint8 a, uint8 b) internal pure returns (uint8) {
        return a + b;                   // CHECKED: unchecked is lexical
    }
    function u5() public pure returns (uint8) {
        unchecked { return helperAdd(255, 1); }   // still Panic 0x11
    }
    function u6() public pure returns (uint256) {
        uint256 x = 2;
        unchecked { return x ** 256; }  // wraps to 0
    }
    function u7() public pure returns (uint8) {
        uint8 x = 200;
        unchecked { x *= 2; }           // 400 % 256 = 144
        return x;
    }
    function u8() public pure returns (int8) {
        int8 x = -128;
        unchecked { x = x - 1; }        // wraps to 127
        return x;
    }
}
