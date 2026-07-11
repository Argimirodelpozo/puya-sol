// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function postInc8(uint8 a) external pure returns (uint8) { uint8 x=a; x++; return x; }
    function preInc8(uint8 a) external pure returns (uint8) { uint8 x=a; ++x; return x; }
    function plusEq8(uint8 a) external pure returns (uint8) { uint8 x=a; x+=1; return x; }
    function plain8(uint8 a) external pure returns (uint8) { uint8 x=a; x=x+1; return x; }
    function postInc16(uint16 a) external pure returns (uint16) { uint16 x=a; x++; return x; }
    function postInc64(uint64 a) external pure returns (uint64) { uint64 x=a; x++; return x; }
    function postInc128(uint128 a) external pure returns (uint128) { uint128 x=a; x++; return x; }
    function postIncI8(int8 a) external pure returns (int8) { int8 x=a; x++; return x; }
    function postDec8(uint8 a) external pure returns (uint8) { uint8 x=a; x--; return x; }
}
