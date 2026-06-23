// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--cast).
// Unchecked sub-256 biguint (uint65..uint248) SUBTRACTION (underflow) and EXPONENTIATION wrapped to
// 2^256 (buildWrappingSubtract / buildBigUIntExp use mod 2^256) instead of the type width 2^N: an
// unchecked `uint128(0) - 1` was 2^256-1 not 2^128-1, and `uint128 a ** 2` kept the full product. The
// return path re-masked, so it only surfaced when consumed — `<= type(uint128).max` returned the WRONG
// boolean (soundness), and checked consumers would false-revert. FIX: mask the unchecked unsigned
// sub-256 biguint sub/exp result to 2^N (the uint64 mul/add wrap and the v427/v428/v429 fixes are the
// same canonicalisation class). Mul/Add look correct for a STANDALONE return (re-masked at encode) but
// are wrong once consumed — fixed separately, see unchecked_biguint_muladd_consumed; uint256 keeps 2^256.
contract UncheckedBiguintSubExpWrap {
    function usub(uint128 a, uint128 b) external pure returns (uint128) { unchecked { return a - b; } }   // 0-1 == 2^128-1
    function uexp(uint128 a)            external pure returns (uint128) { unchecked { return a ** 2; } }   // (2^64)^2 == 0 mod 2^128
    function usubCmp(uint128 a, uint128 b) external pure returns (bool) { unchecked { return (a - b) <= type(uint128).max; } } // true
    function uexpCmp(uint128 a)         external pure returns (bool)    { unchecked { return (a ** 2) <= type(uint128).max; } } // true
    function usub200(uint200 a, uint200 b) external pure returns (uint200) { unchecked { return a - b; } } // width-general
    function uexp160(uint160 a)         external pure returns (uint160) { unchecked { return a ** 2; } }
    // checked must still revert on under/overflow
    function csub(uint128 a, uint128 b) external pure returns (uint128) { return a - b; }
    function cexp(uint128 a)            external pure returns (uint128) { return a ** 2; }
    // standalone unchecked add/mul re-mask at encode (the CONSUMED case is fixed in
    // unchecked_biguint_muladd_consumed); keep these as a guard standalone stays correct
    function uadd(uint128 a, uint128 b) external pure returns (uint128) { unchecked { return a + b; } }
    function umul(uint128 a, uint128 b) external pure returns (uint128) { unchecked { return a * b; } }
}
