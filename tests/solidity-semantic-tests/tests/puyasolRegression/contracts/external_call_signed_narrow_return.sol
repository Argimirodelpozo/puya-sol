// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Signed sub-word in a STRUCT field / ARRAY element across a call: the callee names it `int64`
// (canonical, sign-preserved) but the caller (SolExternalCall) used wtypeToABIName which dropped the
// sign to `uint64` -> selector mismatch -> router err. Fixed by routing the caller's aggregate naming
// through the canonical nestedArc4Name (same namer the callee/puya use).
struct Pair { int64 px; int64 py; }
// Regression: a SIGNED narrow-int (int8/16/32/64) RETURN from an external/inner contract call.
// The callee encodes a signed int return as a 32-byte uint256 (sign-extended), but the caller used
// to decode it with an 8-byte `btoi` → "btoi arg too long, got 32 bytes" → revert on EVERY such call
// (value-independent). Fixed by extracting the low 8 bytes (the canonical uint64-backed form) before
// btoi when the Solidity return type is signed. (Found by the cross-contract differential fuzzer.)
// Forwards widen+offset so the observable is a clean positive int; a broken decode reverts instead.
contract Cee {
    function r8(int256 a)  external pure returns (int8)  { return int8(a); }
    function r16(int256 a) external pure returns (int16) { return int16(a); }
    function r32(int256 a) external pure returns (int32) { return int32(a); }
    function r64(int256 a) external pure returns (int64) { return int64(a); }
    function u32(uint256 a) external pure returns (uint32) { return uint32(a); }   // unsigned control
    // signed-narrow TUPLE return: callee used to name these uint512 (vs caller uint256) → selector
    // mismatch → router err. Now named uint256 on both sides.
    function pair(int64 a, int64 b) external pure returns (int64, int64) { return (a, b); }
    function mixed(int64 a, uint64 b) external pure returns (int64, uint64) { return (a, b); }
    // UNSIGNED biguint (uint128/uint256) in a TUPLE return: encoded at natural N/8-byte width (16B for
    // uint128), but the caller's tuple decode used a fixed 32B field -> wrong offsets -> revert.
    function p128(uint128 a, uint128 b) external pure returns (uint128, uint128) { return (a, b); }
    function pmix(uint128 a, uint64 b) external pure returns (uint128, uint64) { return (a, b); }
    // signed STRUCT-field / ARRAY-element returns + args (callee names them int64 vs caller's old uint64).
    function packP(int64 a, int64 b) external pure returns (Pair memory) { return Pair(a, b); }
    function unpackP(Pair memory p) external pure returns (int64) { unchecked { return p.px + p.py; } }
    function mkArr(int64 a) external pure returns (int64[] memory) { int64[] memory r = new int64[](2); r[0]=a; r[1]=a; return r; }
    function sumArr(int64[] memory xs) external pure returns (int64) { int64 s; unchecked { for(uint i;i<xs.length;i++) s+=xs[i]; } return s; }
}
contract Caller {
    Cee c;
    constructor() { c = new Cee(); }
    function g8(int256 a)  external returns (int256) { return int256(c.r8(a))  + 1000; }
    function g16(int256 a) external returns (int256) { return int256(c.r16(a)) + 1000; }
    function g32(int256 a) external returns (int256) { return int256(c.r32(a)) + 1000; }
    function g64(int256 a) external returns (int256) { return int256(c.r64(a)) + 1000; }
    function gu32(uint256 a) external returns (uint256) { return uint256(c.u32(a)) + 1000; }
    // tuple-return forwards: widen+offset so the observable is a clean positive int.
    function gpair(int64 a, int64 b) external returns (int256) {
        (int64 x, int64 y) = c.pair(a, b);
        return int256(x) + int256(y) + 1000;
    }
    function gmixed(int64 a, uint64 b) external returns (int256) {
        (int64 x, uint64 y) = c.mixed(a, b);
        return int256(x) + int256(uint256(y)) + 1000;
    }
    function g128(uint128 a, uint128 b) external returns (uint256) {
        (uint128 x, uint128 y) = c.p128(a, b);
        return uint256(x) + uint256(y);
    }
    function gpmix(uint128 a, uint64 b) external returns (uint256) {
        (uint128 x, uint64 y) = c.pmix(a, b);
        return uint256(x) + uint256(y);
    }
    // struct return then struct arg re-passed; widen+offset for a clean positive observable.
    function gStruct(int64 a, int64 b) external returns (int256) {
        Pair memory p = c.packP(a, b);
        return int256(c.unpackP(p)) + 1000;
    }
    // array return then array arg re-passed.
    function gArr(int64 a) external returns (int256) {
        return int256(c.sumArr(c.mkArr(a))) + 1000;
    }
}
