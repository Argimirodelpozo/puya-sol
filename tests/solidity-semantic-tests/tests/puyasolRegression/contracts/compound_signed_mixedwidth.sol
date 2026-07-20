// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the compound-assign residual of the signed mixed-width division family:
// `x /= y` with a biguint-backed signed LHS (int72..int256) and a NARROWER
// signed divisor built the RHS at the TARGET type, so a negative int16 divisor
// was sign-extended from bit 127 (no-op) and read as +1.8e19 → x became 0
// instead of 256. The plain-division form `x / y` extended from the divisor's
// own width and was correct — a classic one-twin-fixed drift.
contract CompoundSignedMixedWidth {
    function compoundDiv(int128 x, int16 y) external pure returns (int128) {
        x /= y;
        return x;
    }

    function compoundMod(int128 x, int16 y) external pure returns (int128) {
        x %= y;
        return x;
    }

    function plainDiv(int128 x, int16 y) external pure returns (int128) {
        return x / y;
    }

    // ≤64-bit target tier: int32 /= int8 (both uint64-carried).
    function compoundDivSmall(int32 x, int8 y) external pure returns (int32) {
        x /= y;
        return x;
    }

    // sub-compound ops with a widened negative RHS
    function compoundSub(int128 x, int16 y) external pure returns (int128) {
        x -= y;
        return x;
    }

    function compoundMul(int128 x, int16 y) external pure returns (int128) {
        x *= y;
        return x;
    }
}
