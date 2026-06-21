// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--cast).
// Checked `-(type(intN).min)` overflows and must REVERT (the result 2^(N-1) doesn't fit intN);
// `unchecked` wraps it back to intN.min. The checked-overflow guard in SolIntegerBuilder::unary_op
// missed exactly int64 and int128:
//   - int64: the mask `(uint64_t(1) << 64) - 1` is C++ UB (shift == width) -> 0, so the guard
//     compared `operand & 0 == 0` to 2^63 and never fired.
//   - int128 (any biguint-backed sub-256 signed): the operand is the 256-bit sign-extended TC, so
//     int128.min reads as 2^256 - 2^127, but the guard compared against 2^127 -> never equal.
// int8/16/32 (narrower uint64-backed) and int256 (2^256-2^255 == 2^255) already reverted correctly.
contract SignedNegationOverflow {
    function neg8(int8 x) external pure returns (int8) { return -x; }
    function neg16(int16 x) external pure returns (int16) { return -x; }
    function neg32(int32 x) external pure returns (int32) { return -x; }
    function neg64(int64 x) external pure returns (int64) { return -x; }
    function neg128(int128 x) external pure returns (int128) { return -x; }
    function neg256(int256 x) external pure returns (int256) { return -x; }
    function uneg64(int64 x) external pure returns (int64) { unchecked { return -x; } }
    function uneg128(int128 x) external pure returns (int128) { unchecked { return -x; } }
}
