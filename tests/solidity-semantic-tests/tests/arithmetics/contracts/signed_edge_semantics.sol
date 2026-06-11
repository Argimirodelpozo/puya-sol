// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    int256 constant MIN = type(int256).min;
    // EVM truncated division/modulo (toward zero)
    function divTrunc() external pure returns (int256, int256, int256, int256) {
        int256 a = -7; int256 b = 2;
        int256 c = 7;  int256 d = -2;
        return (a / b, c / d, a % 3, c % -3);   // (-3, -3, -1, 1)
    }
    // INT_MIN % -1 == 0 (NO panic; only div panics)
    function minModMinus1() external pure returns (int256) {
        int256 m = MIN; int256 n = -1;
        return m % n;   // 0
    }
    // unchecked INT_MIN / -1 == INT_MIN (EVM sdiv overflow wrap)
    function uncheckedMinDiv() external pure returns (int256) {
        int256 m = MIN; int256 n = -1;
        unchecked { return m / n; }   // MIN
    }
    // signed exponent semantics
    function sexp() external pure returns (int256, int256, int256) {
        int256 b2 = -2;
        return (b2 ** 3, b2 ** 2, int256(0) ** 0);   // (-8, 4, 1)
    }
    // signed shifts: arithmetic right, fill sign; left shifts mod 2^256
    function sshift(int256 x, uint256 s) external pure returns (int256) { return x >> s; }
    function sshiftBig() external pure returns (int256, int256) {
        int256 neg = -8;
        return (neg >> 300, int256(8) >> 300);   // (-1, 0) — shift >= 256 saturates
    }
    // compound signed shift (compound bypasses SolBinaryOperation routing!)
    function compoundSar() external pure returns (int256) {
        int256 x = -8;
        x >>= 1;
        return x;   // -4 (arithmetic, not logical)
    }
    function compoundShl() external pure returns (int256) {
        int256 x = -4;
        x <<= 1;
        return x;   // -8
    }
    // unchecked unary minus MIN wraps to MIN
    function uncheckedNegMin() external pure returns (int256) {
        int256 m = MIN;
        unchecked { return -m; }   // MIN
    }
    // mixed-width compound on int128 (canonicalization)
    function compoundNarrow() external pure returns (int128) {
        int128 x = -100;
        x %= 7;     // -2
        return x;
    }
}
