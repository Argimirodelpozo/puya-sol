// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Guard for SIGNED narrowing intM -> intN (N>64, e.g. OZ/V4 SafeCast.toInt128).
// `int128(value)` must produce the canonical 256-bit two's-complement (sign-
// extended from N), so an in-range NEGATIVE round-trips (`downcasted == value`)
// instead of comparing the low-N-bit form against the full-width value and
// wrongly reverting. Out-of-range values must still revert.
contract SignedNarrowing {
    function narrow(int256 v) external pure returns (int128) { return int128(v); }

    // SafeCast.toInt128 pattern
    function toI128(int256 v) external pure returns (int128) {
        int128 d = int128(v);
        require(d == v, "overflow");
        return d;
    }
}
