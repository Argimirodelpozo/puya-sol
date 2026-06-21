// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the GENERATIVE fuzzer's ARRAY mode
// (fuzz_gen.py --arr), isolated from an int16 all-INT_MIN divergence. An unchecked unary minus on a
// sub-word signed value computed the full-width result but did NOT wrap to the N-bit range:
// -INT_MIN = +2^(N-1), which overflows intN and must wrap back to INT_MIN. The bare-return path
// re-truncates, so `-a` alone looked right; as a subexpression feeding a signed compare (whose
// XOR-sign-bit trick assumes canonical operands) the raw +2^(N-1) read as positive. Fix masks to N
// bits + sign-extends the negation result (uint64 path for N<64, 256-bit path for 64<N<256).
contract SignedSubwordNegate {
    function cmp8(int8 a)     external pure returns (bool)  { unchecked { return (-a) > a; } }
    function cmp16(int16 a)   external pure returns (bool)  { unchecked { return (-a) > a; } }
    function cmp128(int128 a) external pure returns (bool)  { unchecked { return (-a) > a; } }
    function neg16(int16 a)   external pure returns (int16) { unchecked { return -a; } }  // bare: re-truncates
    function negc16(int16 a)  external pure returns (int16) { return -a; }                // checked: INT_MIN reverts
}
