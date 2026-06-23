// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (seed 20006).
// Unchecked sub-256 biguint (uint65..248) Add/Mult wrapped the result to 2^256 (wrapMod256), not the
// type width 2^N. INVISIBLE for a standalone `return a*b` (the ARC4 encode re-masks to 2^N), so the
// sibling unchecked_biguint_sub_exp_wrap guards said add/mul were "already correct". But once the
// non-canonical (>2^N, <2^256) intermediate is CONSUMED it is WRONG: `(a * ~c) / x` divided a too-wide
// dividend. FIX: mask the unchecked unsigned sub-256 Add/Mult result to 2^N (maskUnsignedToWidth),
// matching the unchecked sub/exp class; uint256 keeps the full 2^256 wrap.
contract C {
    // fuzzer f18 @seed 20006: (a * ~c) / ((c<<0) ^ a). For (2,0): (2*(2^128-1) mod 2^128)/2
    // = (2^128-2)/2 = 2^127-1 — NOT 2^128-1 (which is what the unwrapped 2^129-2 dividend gives).
    function mulDiv(uint128 a, uint128 c) external pure returns (uint128) {
        unchecked { return (a * (~c)) / ((c << 0) ^ a); }
    }
    // unchecked add overflow consumed by a divide: (2^128-1 + 1)/2 = (0)/2 = 0 — NOT 2^127.
    function addDiv(uint128 a, uint128 b, uint128 d) external pure returns (uint128) {
        unchecked { return (a + b) / d; }
    }
    // width-general (uint200): (2 * ~0 mod 2^200)/2 = (2^200-2)/2 = 2^199-1.
    function mul200(uint200 a, uint200 c) external pure returns (uint200) {
        unchecked { return (a * (~c)) / (c ^ a); }
    }
    // standalone return must STILL wrap correctly (re-masked at encode): 2^64 * 2^64 == 0 mod 2^128.
    function umul(uint128 a, uint128 b) external pure returns (uint128) { unchecked { return a * b; } }
}
