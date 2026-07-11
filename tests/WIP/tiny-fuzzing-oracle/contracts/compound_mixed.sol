// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // compound where target and operand differ in width/sign (coercion paths)
    function di8byi16(int8 a, int16 b) external pure returns (int8) { int8 x=a; x /= int8(b); return x; }
    function mi64byi8(int64 a, int8 b) external pure returns (int64) { int64 x=a; x %= int64(b); return x; }
    function divNeg32(int32 a, int32 b) external pure returns (int32) { int32 x=a; x /= b; return x; }
}
