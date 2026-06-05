// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// NOTE: puya-sol CUSTOM regression test — NOT part of the upstream Solidity
// semantic-tests suite (added to guard a puya-sol-specific codegen path).

// Guard for the V4 SqrtPriceMath.getAmount0/1Delta(int128 liquidity) NEGATIVE
// branch (remove-liquidity). The signed `liquidity < 0` ordering compare lowers
// to "XOR both sides with 2^255" (the 256-bit sign bit). That is only correct
// when a negative int128 is held in CANONICAL 256-bit two's complement
// (2^256 - X, bit 255 set). If a sub-256 negative arrives as a 128-bit two's
// complement (2^128 - X, bit 127 set / bit 255 clear), `x < 0` returns FALSE,
// the remove takes the ADD branch, and `uint128(liquidity)` becomes ~2^128 — a
// garbage amount (the observed ~1.76e19 take underflow). This is the remove
// counterpart to the add-path int24/int128 sign-extension fixes (#49/#50).
contract SignedInt128Neg {
    // ---- param-origin (ABI-decoded int128) -------------------------------
    function isNeg(int128 x) external pure returns (bool) {
        return x < 0;
    }

    // exact magnitude-extraction shape: uint128(-x) for x<0, else uint128(x)
    function mag(int128 x) external pure returns (uint128) {
        return x < 0 ? uint128(-x) : uint128(x);
    }

    // the full getAmount*Delta(int128) ternary shape, returning a signed int256
    function branch(int128 x) external pure returns (int256) {
        return x < 0
            ? int256(uint256(uint128(-x)))
            : -int256(uint256(uint128(x)));
    }

    // ---- computed-origin (negative int128 from a subtraction) -------------
    // liquidityDelta is frequently a difference; verify the sign survives.
    function isNegSub(int128 a, int128 b) external pure returns (bool) {
        int128 d = a - b;
        return d < 0;
    }

    function magSub(int128 a, int128 b) external pure returns (uint128) {
        int128 d = a - b;
        return d < 0 ? uint128(-d) : uint128(d);
    }

    // ---- round-trip-origin (stored to / read from a state int128) ---------
    int128 stored;

    function storeNeg(int128 x) external {
        stored = x;
    }

    function isNegStored() external view returns (bool) {
        return stored < 0;
    }

    function magStored() external view returns (uint128) {
        return stored < 0 ? uint128(-stored) : uint128(stored);
    }
}
