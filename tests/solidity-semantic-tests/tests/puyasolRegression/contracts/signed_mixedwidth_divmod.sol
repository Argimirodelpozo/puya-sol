// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (mixed-width arithmetic).
// SIGNED division/modulo where the dividend is biguint-backed (int128/int256) and the divisor is a
// NARROWER signed type returned garbage: buildSignedDivMod masks both operands to N (commonType) bits
// and reads sign via `>= 2^(N-1)`, but a narrow divisor (int16 -32768) arrives sign-extended only in its
// own 64-bit slot (2^64-32768), which masks to a huge POSITIVE N-bit value -> wrong abs/sign (int128/int16
// div gave 0, mod gave the dividend). FIX: coerceToCommonInt each operand to canonical commonType (sign-
// extend from its own width) before buildSignedDivMod. Clean when divisor==dividend width / uint64-backed
// dividend / unsigned.
contract SignedMixedwidthDivmod {
    function div128_16(int128 x, int16 y) external pure returns (int128) { unchecked { return x / y; } }
    function mod128_16(int128 x, int16 y) external pure returns (int128) { unchecked { return x % y; } }
    function div128_8(int128 x, int8 y) external pure returns (int128) { unchecked { return x / y; } }
    function div256_16(int256 x, int16 y) external pure returns (int256) { unchecked { return x / y; } }
    function mod256_16(int256 x, int16 y) external pure returns (int256) { unchecked { return x % y; } }
}
