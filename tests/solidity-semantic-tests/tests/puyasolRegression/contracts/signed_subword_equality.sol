// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--arr, seed 13004 f0).
// Two non-canonical-operand bugs in equality on sub-word signed ints (the == / != analogue of the
// ordering-compare fix):
//   1. A negative LITERAL cast `int8(-1)` was emitted as the bare value 255 (0xff) — masked but not
//      sign-extended — so `int8(-1) == int8(-1)` compared 255 against a canonical -1 and returned FALSE.
//      Fixed at the source (SolTypeConversion: sign-extend any signed sub-word cast result from its width).
//   2. An unchecked sub-word ARITHMETIC result (e.g. `acc = 127; acc -= -128` wraps to -1 but as 0xff)
//      compared `== nonzero` wrongly, because SolIntegerBuilder::compare only sign-extended operands for
//      ORDERING ops, not equality. Fixed by canonicalising operands for both ordering AND equality.
// Comparing to 0 hid both (0 is canonical either way); only `== nonzero` exposes them.
contract SignedSubwordEquality {
    function eqNegLit8() external pure returns (bool) { return int8(-1) == int8(-1); }
    function eqNegLit16() external pure returns (bool) { return int16(-1) == int16(-1); }
    function eqParamLit(int8 a) external pure returns (bool) { return a == int8(-1); }
    // unchecked arith result compared to a non-zero value
    function eqAfterSub(int8 x, int8 y) external pure returns (bool) {
        int8 acc = x; unchecked { acc -= y; return acc == -(acc ** 0); }   // acc == -1 when acc wrapped to -1
    }
    function neAfterAdd(int16 x, int16 y) external pure returns (bool) {
        int16 acc = x; unchecked { acc += y; return acc != int16(-1); }
    }
    // sanity: canonical equality unaffected, and int64 (full width) correct
    function eqParam(int8 a, int8 b) external pure returns (bool) { return a == b; }
    function eqNegLit64() external pure returns (bool) { return int64(-1) == int64(-1); }
    function eqNegLit256() external pure returns (bool) { return int256(-1) == int256(-1); }
}
