// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found while applying solc-todo opportunity D's residual.
// A mixed-width bitwise op (`&` / `|` / `^`) with a narrower SIGNED operand mis-evaluated: the narrower
// operand was reinterpreted at the common width WITHOUT sign-extension, so `int128(-1) & int16(-32768)`
// ANDed the raw 16-bit 0x8000 (= +32768) instead of the sign-extended int128 value 0x..FF8000 (= -32768).
// Both the narrower-LEFT and narrower-RIGHT positions were wrong. FIX: drive operand conversion off
// solc's commonType — coerce BOTH integer operands to commonType (canonicalising / sign-extending),
// mirroring the comparison path. Shifts (right = shift amount) and unsigned operands are unaffected.
contract MixedWidthSignedBitwise {
    function andSL(int16 a, int128 b)  external pure returns (int128) { return a & b; }   // narrower left
    function andSR(int128 a, int16 b)  external pure returns (int128) { return a & b; }   // narrower right
    function orSL(int16 a, int128 b)   external pure returns (int128) { return a | b; }
    function orSR(int128 a, int16 b)   external pure returns (int128) { return a | b; }
    function xorSR(int128 a, int16 b)  external pure returns (int128) { return a ^ b; }
    function andSL8(int8 a, int256 b)  external pure returns (int256) { return a & b; }
    function addU(uint16 a, uint128 b) external pure returns (uint128) { return a + b; }  // unsigned unaffected
}
