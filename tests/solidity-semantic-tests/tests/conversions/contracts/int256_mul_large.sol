// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// NOTE: puya-sol CUSTOM regression test — NOT part of the upstream Solidity
// semantic-tests suite (added to guard a puya-sol-specific codegen path).

// Repro of the V4 TickMath.getTickAtSqrtPrice log2 step the xfail blames:
//   int256 log_sqrt10001 = log_2 * 255738958999603826347141;
// log_2 is a large NEGATIVE int256 at MIN_SQRT_PRICE; the product (~2^148) is a
// valid int256. Guards signed int256 multiplication with a large negative operand
// (the raw biguint product ~2^334 must wrap mod 2^256 to the right two's complement,
// without spuriously tripping the overflow check).
contract Int256MulLarge {
    function f(int256 a) external pure returns (int256) {
        return a * 255738958999603826347141;
    }
}
