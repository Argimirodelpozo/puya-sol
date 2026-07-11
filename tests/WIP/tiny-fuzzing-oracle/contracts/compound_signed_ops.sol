// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Cousins of the compound-signed-div/mod bug: other compound ops on uint64-backed signed types
// that route through SolIntegerBuilder::binary_op (the eb compound path).
contract C {
    function shrI8(int8 a, uint8 n) external pure returns (int8) { int8 x=a; x >>= (n % 9); return x; }   // arithmetic
    function shlI8(int8 a, uint8 n) external pure returns (int8) { int8 x=a; unchecked { x <<= (n % 9); } return x; }
    function shrI64(int64 a, uint8 n) external pure returns (int64) { int64 x=a; x >>= (n % 65); return x; }
    function andI8(int8 a, int8 b) external pure returns (int8) { int8 x=a; x &= b; return x; }
    function orI16(int16 a, int16 b) external pure returns (int16) { int16 x=a; x |= b; return x; }
    function xorI64(int64 a, int64 b) external pure returns (int64) { int64 x=a; x ^= b; return x; }
    function subI8(int8 a, int8 b) external pure returns (int8) { int8 x=a; unchecked { x -= b; } return x; }
    function mulI8(int8 a, int8 b) external pure returns (int8) { int8 x=a; unchecked { x *= b; } return x; }
    function addI16(int16 a, int16 b) external pure returns (int16) { int16 x=a; unchecked { x += b; } return x; }
    // div/mod in ternary + nested (non-storage compound paths)
    function ternDiv(int8 a, int8 b, bool c) external pure returns (int8) { return c ? (a / b) : (b / (a==0?int8(1):a)); }
}
