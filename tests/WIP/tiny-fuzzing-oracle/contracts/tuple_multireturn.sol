// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function mk2(int64 a, int64 b) internal pure returns (int64, int64) { return (a+1, b-1); }
    function swap2(uint128 a, uint128 b) external pure returns (uint128, uint128) {
        (a, b) = (b, a);
        return (a, b);
    }
    function destr(int64 x, int64 y) external pure returns (int64) {
        (int64 p, int64 q) = mk2(x, y);
        return p * q;
    }
    function partDestr(uint64 a, uint64 b, uint64 c) external pure returns (uint64) {
        uint64 r;
        (, r, ) = (a, b, c);
        return r;
    }
    function nestedTuple(int32 a, int32 b) external pure returns (int32, int32, int32) {
        (int32 x, int32 y) = (a, b);
        return (x + y, x - y, x * y);
    }
    function mk3(uint8 a) internal pure returns (uint8, uint8, uint8) { return (a, a*2, a*3); }
    function use3(uint8 a) external pure returns (uint8) {
        (uint8 p, uint8 q, uint8 r) = mk3(a);
        unchecked { return p + q + r; }
    }
    // tuple swap with mixed sign
    function swapMixed(int128 a, uint128 b) external pure returns (uint128, int128) {
        int128 tmp = a;
        return (b, tmp);
    }
    // chained assignment
    function chained(uint64 v) external pure returns (uint64, uint64) {
        uint64 a; uint64 b;
        a = b = v;
        return (a, b);
    }
}
