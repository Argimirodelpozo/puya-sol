// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// CUSTOM puya-sol test contract (NOT vendored from the upstream Solidity
// semantic suite) — added by us to guard a compiler fix. A signed sub-256
// array element (e.g. int128) is decoded as its raw N-bit two's complement;
// it must be sign-extended to the canonical 256-bit form on read so that
// `a[i]` compares/arithmetics equal to a sign-extended scalar of the same
// value. Before the fix, eq0([-777], -777) returned false (the element was
// 2^128-777, the scalar 2^256-777).
contract Int128ArrCheck {
    function ident(int128 x) external pure returns (int128) { return x; }          // scalar round-trip
    function get0(int128[] calldata a) external pure returns (int128) { return a[0]; } // elem return
    function eq0(int128[] calldata a, int128 t) external pure returns (bool) { return a[0] == t; } // compare
    function gt0(int128[] calldata a) external pure returns (bool) { return a[0] < 0; } // sign test
    // Arithmetic across two decoded signed elements (widened to int256).
    function sum2(int128[] calldata a) external pure returns (int256) {
        return int256(a[0]) + int256(a[1]);
    }
    // Memory array: a different element access path than calldata. Stores a
    // negative literal, reads it back and compares to the scalar t (true iff
    // t == -5), so the read must sign-extend a[0] to match.
    function eqMem(int128 t) external pure returns (bool) {
        int128[] memory a = new int128[](2);
        a[0] = -5;
        a[1] = 99;
        return a[0] == t;
    }
}
