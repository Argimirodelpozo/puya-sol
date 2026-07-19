// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc48000_11).
struct Scc48000_11 { int16 m0; int128 m1; bytes4 m2; }
contract Cee_cc48000_11 {
    function f1(int256 a) external pure returns (int8) { return int8(a); }
    function f2(int256 a) external pure returns (int16) { return int16(a); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (uint32, int8, int16) { return (uint32(uint256(a0)), int8(a1), int16(a2)); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (uint64, int16, uint32) { return (uint64(uint256(a0)), int16(a1), uint32(uint256(a2))); }
    function f5(int256 a0, int256 a1, int256 a2) external pure returns (Scc48000_11 memory) { return Scc48000_11(int16(a0), int128(a1), bytes4(uint32(uint256(a2)))); }
    function f5x(Scc48000_11 memory p) external pure returns (int256) { return int256(p.m0) + int256(p.m1) + int256(uint256(uint32(p.m2))); }
    function f6(int256 a) external pure returns (int64[] memory) { int64[] memory r = new int64[](2); r[0] = int64(a + int256(0)); r[1] = int64(a + int256(1)); return r; }
    function f6x(int64[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f7(int8 x, int8 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc48000_11 {
    Cee_cc48000_11 c;
    constructor() { c = new Cee_cc48000_11(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint32 x0, int8 x1, int16 x2) = c.f3(a0, a1, a2); return int256(uint256(x0)) + int256(x1) + int256(x2); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint64 x0, int16 x1, uint32 x2) = c.f4(a0, a1, a2); return int256(uint256(x0)) + int256(x1) + int256(uint256(x2)); }
    function gf5(int256 a0, int256 a1, int256 a2) external view returns (int256) { Scc48000_11 memory p = c.f5(a0, a1, a2); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int8(a), int8(b)); }
}
