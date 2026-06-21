// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--cf).
// A signed ORDERING comparison (`<` `<=` `>` `>=`) on a sub-word int (int8/16/32) read the wrong
// result whenever an operand was NOT already sign-extended in its uint64 slot — i.e. a negative
// literal cast (`int8(-1)` = 0xff, low N bits only) or an unchecked sub-word arithmetic result
// (`0 - (-128)` = 0x80). SolIntegerBuilder::compare's uint64 path XOR'd each operand with 2^63 to
// flip to unsigned ordering but never sign-extended first, so 0xff (-1) ordered ABOVE 0x80…00 (0).
// ABI params arrive sign-extended, so the suite's variable-based comparisons hid it. int64 (full
// width) and int256 (biguint path, which did sign-extend) were already correct.
// FIX: signExtendToUint64 each signed operand before the 2^63 XOR (mirrors the biguint branch's
// signExtendToUint256); idempotent for canonical operands, no-op for int64.
contract SignedSubwordCompare {
    // negative literal cast compared — the minimal repro (was false)
    function ltNeg8() external pure returns (bool) { return int8(-1) < int8(0); }
    function ltNeg16() external pure returns (bool) { return int16(-1) < int16(0); }
    function ltNeg32() external pure returns (bool) { return int32(-1) < int32(0); }
    function ltNeg256() external pure returns (bool) { return int256(-1) < int256(0); }
    // unchecked sub-word arithmetic result compared to 0
    function modNeg8(int8 b, int8 d) external pure returns (bool) { unchecked { return (b % d) < int8(0); } }
    function subWrap8(int8 b, int8 d) external pure returns (bool) { unchecked { return (b - d) < int8(0); } }
    function mulWrap8(int8 b, int8 d) external pure returns (bool) { unchecked { return (b * d) < int8(0); } }
    function modNeg16(int16 b, int16 d) external pure returns (bool) { unchecked { return (b % d) < int16(0); } }
    function modNeg64(int64 b, int64 d) external pure returns (bool) { unchecked { return (b % d) < int64(0); } }
    // sanity: ordinary param comparison unaffected
    function ltPos8(int8 a, int8 b) external pure returns (bool) { return a < b; }
    function gte16(int16 a, int16 b) external pure returns (bool) { return a >= b; }
}
