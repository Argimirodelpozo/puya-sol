// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--cast).
// A same-width signed->unsigned biguint cast `uintN(intN(x))` (65 <= N <= 248) of a NEGATIVE intN left
// the value in its canonical 256-bit two's-complement form (int128(-1) == 2^256-1) instead of trimming
// to N bits (uint128 of it is 2^128-1). The RETURN path re-canonicalised, so it only surfaced when the
// cast result was consumed: checked `** 1` / `* 1` / `+ 0` false-reverted (2^256-1 > uintN.max), and a
// comparison `<= type(uintN).max` returned the WRONG boolean (soundness). FIX: applyNarrowingMask now
// masks to 2^N for a signed-source/unsigned-target biguint cast even at equal width (uint256 keeps the
// full width: uint256(int256(-1)) IS 2^256-1).
contract SignedToUnsignedCastTrim {
    function castPow(uint128 c)  external pure returns (uint128) { return uint128(int128(c)) ** 1; }   // == c
    function castMul(uint128 c)  external pure returns (uint128) { return uint128(int128(c)) * 1; }     // == c
    function castAdd(uint128 c)  external pure returns (uint128) { return uint128(int128(c)) + 0; }     // == c
    function castCmp(uint128 c)  external pure returns (bool)    { return uint128(int128(c)) <= type(uint128).max; } // true
    function cast160(uint160 c)  external pure returns (uint160) { return uint160(int160(c)) + 0; }     // == c
    function castNarrow(uint256 c) external pure returns (uint128) { return uint128(int128(int256(c))) ** 1; } // low 128 bits
    // sanity: uint256(int256(-1)) keeps the full 256-bit width (unaffected by the fix)
    function u256ofI256(int256 c) external pure returns (uint256) { return uint256(c); }
}
