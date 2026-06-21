// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Locks type(intN).min/max emission after the
// solc-reuse consolidation: SolMetaTypeAccess now routes solc's IntegerType::min()/max() (256-bit
// two's-complement u256) through the shared TypeCoercion::canonicalIntConstant helper (<=64-bit ->
// low 64-bit TC / uint64; >64 -> 256-bit TC / biguint), replacing the hand-rolled TC math that had
// to stay in lockstep with SolLiteral. These must keep comparing/arithmetic-ing equal to literals.
contract TypeMinMaxCanonical {
    function mn8() external pure returns (int8) { return type(int8).min; }      // -128
    function mn16() external pure returns (int16) { return type(int16).min; }   // -32768
    function mn128() external pure returns (int128) { return type(int128).min; }
    function mn256() external pure returns (int256) { return type(int256).min; }
    function mx8() external pure returns (int8) { return type(int8).max; }       // 127
    function mxu256() external pure returns (uint256) { return type(uint256).max; }
    // canonical form must compare equal to the matching literal (the "must line up" cases)
    function minIsLit8(int8 a) external pure returns (bool) { return a == type(int8).min; }
    function minIsLit128(int128 a) external pure returns (bool) { return a == type(int128).min; }
    // and behave in arithmetic
    function addMin8(int8 a) external pure returns (int8) { unchecked { return a + type(int8).min; } }
}
