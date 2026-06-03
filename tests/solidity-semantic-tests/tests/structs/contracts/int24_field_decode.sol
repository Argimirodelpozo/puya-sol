// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Guard for SIGNED sub-word (int24) field DECODE + widening. A packed arc4.intN
// (N<64) field decodes (extract + btoi) to the raw N-bit value; it must be
// sign-extended to the 64-bit two's-complement so casts / widening / comparison
// / arithmetic on the read see the correct NEGATIVE value.
contract Int24FieldDecode {
    struct S { int24 a; int24 b; }

    // implicit widening of field reads to int128 (no explicit cast)
    function implWiden(S memory s) external pure returns (int128) {
        int128 x = s.a; int128 y = s.b; return x + y;
    }
    function to64(S memory s) external pure returns (int64) { return int64(s.a); } // <=64-bit cast
    function cmpLt(S memory s) external pure returns (bool) { return s.a < s.b; }   // signed compare
    function sum24(S memory s) external pure returns (int24) { return s.a + s.b; }  // signed int24 arith
    function to256(S memory s) external pure returns (int256) { return int256(s.a); } // explicit widen
}
