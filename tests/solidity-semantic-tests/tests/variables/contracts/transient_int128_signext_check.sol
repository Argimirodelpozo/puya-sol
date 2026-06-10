// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// CUSTOM puya-sol test contract (NOT vendored from the upstream Solidity
// semantic suite) — added by us to guard a compiler fix. A signed sub-256
// transient variable (int128) is stored as its raw N-bit two's complement and
// must be sign-extended to the canonical 256-bit form on read, so it
// compares/arithmetics equal to a scalar of the same value. Transient storage
// clears per-transaction, so each method writes and reads within one call.
contract C {
    int128 transient t;

    // Write then read back, compare to the scalar (true iff the read
    // sign-extends to match the sign-extended scalar param).
    function roundtrip(int128 v) public returns (bool) {
        t = v;
        return t == v;
    }

    // Read sign-extends, so widening to int256 preserves the sign.
    function widen(int128 v) public returns (int256) {
        t = v;
        return int256(t);
    }
}
