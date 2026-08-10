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
    enum E { A, B, C }
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
    // SIGNED + unsigned-biguint mixed tuple: the unsigned uint128 stays at its NATURAL 16B width
    // even though a signed element is present (Pass 4 widens only the SIGNED element to uint256).
    // A decoder that widened the unsigned biguint to 32B here read the wrong bytes → revert. (fuzzer)
    function sbig(int256 a, uint128 b) external pure returns (int256, uint128) { return (a, b); }
    // biguint (uint128/uint256) mixed with a NON-scalar field (bytesN) in a tuple return: the callee's
    // ReturnRewriter Pass 3 only wrapped biguint elements when the tuple was all-scalar, so the bytesN
    // field left the biguint unwrapped → puya named it "uint512" → cross-contract selector mismatch →
    // UNCONDITIONAL revert (any value). Fixed by whole-tuple ARC4Encode. Both field orders + uint256.
    function bu128(uint256 a) external pure returns (bytes4, uint128) { return (bytes4(uint32(a)), uint128(a)); }
    function u128b(uint256 a) external pure returns (uint128, bytes4) { return (uint128(a), bytes4(uint32(a))); }
    function bu256(uint256 a) external pure returns (bytes4, uint256) { return (bytes4(uint32(a)), a); }
    // signed STRUCT-field / ARRAY-element returns + args (callee names them int64 vs caller's old uint64).
    function packP(int64 a, int64 b) external pure returns (Pair memory) { return Pair(a, b); }
    function unpackP(Pair memory p) external pure returns (int64) { unchecked { return p.px + p.py; } }
    function mkArr(int64 a) external pure returns (int64[] memory) { int64[] memory r = new int64[](2); r[0]=a; r[1]=a; return r; }
    function sumArr(int64[] memory xs) external pure returns (int64) { int64 s; unchecked { for(uint i;i<xs.length;i++) s+=xs[i]; } return s; }
    // BOOL-tuple returns: ARC4 packs CONSECUTIVE bools into bits of one byte, so `(bool,bool)`
    // is 1 byte on the wire, not 2. The caller's tuple decode used a flat 1-byte-per-bool offset
    // → misread the 2nd bool + extract3'd off the end → `(bool,bool)` reverted unconditionally,
    // `(bool,bool,uint256)` gave a wrong 2nd bool and reverted on the uint256. (Found by the
    // cross-contract differential fuzzer.)
    function bb(int256 a0, int256 a1) external pure returns (bool, bool) { return ((a0 % 2 == 0), (a1 % 2 == 0)); }
    function b3(int256 a0, int256 a1, int256 a2) external pure returns (bool, bool, bool) { return ((a0 % 2 == 0), (a1 % 2 == 0), (a2 % 2 == 0)); }
    function bbu(int256 a0, int256 a1, int256 a2) external pure returns (bool, bool, uint256) { return ((a0 % 2 == 0), (a1 % 2 == 0), uint256(a2)); }
    function bub(int256 a0, uint64 a1, int256 a2) external pure returns (bool, uint64, bool) { return ((a0 % 2 == 0), a1, (a2 % 2 == 0)); } // bool run broken by a non-bool
    // bytesN field in a tuple return: extract3 yields generic `bytes`, but the field is sized
    // `bytes[N]` -> the destructuring assignment failed to COMPILE ("assignment target type
    // differs") until the decode retags to the sized type. (Found by the cross-contract fuzzer.)
    function tb4(uint256 a) external pure returns (bytes4, uint64) { return (bytes4(uint32(a)), uint64(a)); }
    function tbb(uint256 a) external pure returns (bool, bytes4) { return ((a % 2 == 0), bytes4(uint32(a))); } // bool then bytesN
    // The manual per-field decoder couldn't decode these tuple field kinds; the ARC4Decode-based
    // rewrite (delegating head/tail/dynamic layout to puya) closes them all: (Found by the fuzzer.)
    //  - enum: named "uint8" by the caller (nestedArc4Name) vs "uint64" by the callee → selector err
    //  - address: right-padded 32B static field the manual decoder mis-handled
    //  - dynamic bytes/string/array in a tuple: head is a 32B tail-offset pointer, not inline data
    function ea(uint256 a) external pure returns (E, uint64) { return (E(a % 3), uint64(a)); }
    function ad(uint256 a) external pure returns (address, uint64) { return (address(uint160(a)), uint64(a)); }
    function dyn(uint256 a) external pure returns (uint64, bytes memory) {
        bytes memory b = new bytes(3); b[0] = 0x11; b[1] = 0x22; b[2] = 0x33; return (uint64(a), b);
    }
    function dstr(uint256 a) external pure returns (string memory, uint64) { return ("hi", uint64(a)); }
    function arr(uint256 a) external pure returns (uint64, uint32[] memory) {
        uint32[] memory r = new uint32[](2); r[0] = uint32(a); r[1] = uint32(a) + 1; return (uint64(a), r);
    }
}
contract Caller {
    Cee c;
    // Cee arrives by address: with `new Cee()` the child binary is EMBEDDED in
    // Caller's program, and the pair sits so close to the AVM 8KB program cap
    // that slot mode's ~0.4KB runtime pushed it over. The regression target is
    // the CALLER-side return decode — unchanged by who deploys the callee.
    constructor(address a) { c = Cee(a); }
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
    function gsbig(int256 a, uint128 b) external returns (int256) {
        (int256 x, uint128 y) = c.sbig(a, b);
        return x + int256(uint256(y));
    }
    function gbu128(uint256 a) external returns (uint256) { (bytes4 x, uint128 y) = c.bu128(a); return uint256(uint32(x)) + uint256(y); }
    function gu128b(uint256 a) external returns (uint256) { (uint128 x, bytes4 y) = c.u128b(a); return uint256(x) + uint256(uint32(y)); }
    function gbu256(uint256 a) external returns (uint256) { (bytes4 x, uint256 y) = c.bu256(a); return uint256(uint32(x)) + y; }
    // struct return then struct arg re-passed; widen+offset for a clean positive observable.
    function gStruct(int64 a, int64 b) external returns (int256) {
        Pair memory p = c.packP(a, b);
        return int256(c.unpackP(p)) + 1000;
    }
    // array return then array arg re-passed.
    function gArr(int64 a) external returns (int256) {
        return int256(c.sumArr(c.mkArr(a))) + 1000;
    }
    // bool-tuple forwards: each bool weighted distinctly so a mis-decoded bit is observable.
    function gbb(int256 a0, int256 a1) external returns (uint256) {
        (bool x0, bool x1) = c.bb(a0, a1);
        return (x0 ? 2 : 0) + (x1 ? 1 : 0);
    }
    function gb3(int256 a0, int256 a1, int256 a2) external returns (uint256) {
        (bool x0, bool x1, bool x2) = c.b3(a0, a1, a2);
        return (x0 ? 4 : 0) + (x1 ? 2 : 0) + (x2 ? 1 : 0);
    }
    function gbbu(int256 a0, int256 a1, int256 a2) external returns (int256) {
        (bool x0, bool x1, uint256 x2) = c.bbu(a0, a1, a2);
        return (x0 ? int256(2) : int256(0)) + (x1 ? int256(1) : int256(0)) + int256(uint256(x2));
    }
    function gbub(int256 a0, uint64 a1, int256 a2) external returns (uint256) {
        (bool x0, uint64 x1, bool x2) = c.bub(a0, a1, a2);
        return (x0 ? 100 : 0) + uint256(x1) + (x2 ? 1 : 0);
    }
    function gtb4(uint256 a) external returns (uint256) {
        (bytes4 x0, uint64 x1) = c.tb4(a);
        return uint256(uint32(x0)) * (1 << 64) + uint256(x1);
    }
    function gtbb(uint256 a) external returns (uint256) {
        (bool x0, bytes4 x1) = c.tbb(a);
        return uint256(uint32(x1)) + (x0 ? 1000000 : 0);
    }
    // enum / address / dynamic-field tuple forwards (closed by the ARC4Decode rewrite).
    function gea(uint256 a) external returns (uint256) { (Cee.E e, uint64 x) = c.ea(a); return uint256(uint8(e)) * 1000 + x; }
    function gad(uint256 a) external returns (uint256) { (address ad, uint64 x) = c.ad(a); return uint256(uint160(ad)) + x; }
    function gdyn(uint256 a) external returns (uint256) { (uint64 x, bytes memory b) = c.dyn(a); return uint256(uint8(b[0])) + b.length * 1000 + x; }
    function gdstr(uint256 a) external returns (uint256) { (string memory s, uint64 x) = c.dstr(a); return bytes(s).length + x; }
    function garr(uint256 a) external returns (uint256) { (uint64 x, uint32[] memory r) = c.arr(a); return uint256(r[0]) + uint256(r[1]) * 1000 + x; }
}
