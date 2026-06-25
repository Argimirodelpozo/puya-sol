// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Found by the overnight campaign (rich storage-mutation sweep). Compound signed /= and %= on a
// uint64-backed type (int8/16/32/64) fell to the NATIVE UNSIGNED uint64 div/mod path (needsBigUInt
// didn't include signed div/mod) -> wrong for negative operands (int64 -1/int64.min gave 1, not 0;
// int16 -32768/-128 gave 0, not 256). Plain a/b was always correct (different code path).
contract C {
    function dI8(int8 a, int8 b) external pure returns (int8) { int8 x=a; x /= b; return x; }
    function mI8(int8 a, int8 b) external pure returns (int8) { int8 x=a; x %= b; return x; }
    function dI16(int16 a, int16 b) external pure returns (int16) { int16 x=a; x /= b; return x; }
    function dI64(int64 a, int64 b) external pure returns (int64) { int64 x=a; x /= b; return x; }
    function uI8(int8 a, int8 b) external pure returns (int8) { int8 x=a; unchecked { x /= b; } return x; }
}
