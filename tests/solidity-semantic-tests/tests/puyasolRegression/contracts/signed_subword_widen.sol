// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the differential fuzzer.
// Widening a SIGNED sub-word int (int8) to a wider SIGNED sub-word int (int16) must SIGN-extend,
// not zero-extend, at every coercion site. int8(-1) widened to int16 is -1 (not 255).
contract SignedSubwordWiden {
    int16 sv;
    struct S { int16 f; }
    S st;
    function explicitCast(int256 x) external pure returns (int256) { return int256(int16(int8(x))); }
    function varDecl(int256 x)      external pure returns (int256) { int8 a = int8(x); int16 b = a; return b; }
    function assignTo(int256 x)     external returns (int256)      { int8 a = int8(x); sv = a; return sv; }
    function arg(int256 x)          external pure returns (int16)  { int8 a = int8(x); return _w(a); }
    function _w(int16 v) internal pure returns (int16) { return v; }
    function structField(int256 x)  external returns (int256)      { int8 a = int8(x); st.f = a; return st.f; }
    // int8 PARAM is ABI-decoded sign-extended-to-64: widening must NOT re-extend/corrupt it.
    function paramWiden(int8 t)     external returns (int256)      { sv = t; return sv; }
}
