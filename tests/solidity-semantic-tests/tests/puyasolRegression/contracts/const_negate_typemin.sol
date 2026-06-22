// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--cast).
// `-type(intN).min` overflows intN (no positive counterpart), and solc REVERTS at runtime. But the
// operand/result are RationalNumberType so the checked-signed path was skipped; the <=64-bit constant
// negation fast-path (SolIntegerBuilder::unary_op) folded it to the wrapped value instead of reverting
// (int128/int256 already reverted via the fall-through, since their value exceeds uint64). FIX: skip the
// fold fast-path for the checked intN.min case so it falls through to the overflow check (revert).
// Unchecked still wraps; runtime `-x` already reverted only at x == intN.min; `-type(intN).max` is fine.
contract ConstNegateTypeMin {
    function negMin8()    external pure returns (int8)   { return -type(int8).min; }
    function negMin16()   external pure returns (int16)  { return -type(int16).min; }
    function negMin64()   external pure returns (int64)  { return -type(int64).min; }
    function negMin128()  external pure returns (int128) { return -type(int128).min; }
    function negMin256()  external pure returns (int256) { return -type(int256).min; }
    function nested()     external pure returns (int16)  { return int16(int256(-type(int16).min)); }
    function tildeMin()   external pure returns (int16)  { return ~(-type(int16).min); }
    function uncheckedMin() external pure returns (int16) { unchecked { return -type(int16).min; } } // wraps -> int16.min
    // must NOT revert / stay correct
    function negMax16()   external pure returns (int16)  { return -type(int16).max; }       // -32767
    function negMinPlus() external pure returns (int16)  { return -(type(int16).min + 1); }  // 32767
    function negConst()   external pure returns (int16)  { return -int16(5); }               // -5
    function negVar(int16 x) external pure returns (int16) { return -x; }                     // runtime; reverts at min
}
