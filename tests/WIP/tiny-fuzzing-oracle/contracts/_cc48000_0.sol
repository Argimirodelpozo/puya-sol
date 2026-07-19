// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc48000_0).
struct Scc48000_0 { int32 m0; uint256 m1; }
contract Cee_cc48000_0 {
    function f1(int256 a) external pure returns (int256) { return int256(a); }
    function f2(int256 a0, int256 a1, int256 a2) external pure returns (uint32, uint128, uint64) { return (uint32(uint256(a0)), uint128(uint256(a1)), uint64(uint256(a2))); }
    function f3(int256 a0, int256 a1) external pure returns (int128, int256) { return (int128(a0), int256(a1)); }
    function f4(int256 a0, int256 a1) external pure returns (Scc48000_0 memory) { return Scc48000_0(int32(a0), uint256(uint256(a1))); }
    function f4x(Scc48000_0 memory p) external pure returns (int256) { return int256(p.m0) + int256(uint256(p.m1)); }
    function f5(int256 a) external pure returns (int16[] memory) { int16[] memory r = new int16[](3); r[0] = int16(a + int256(0)); r[1] = int16(a + int256(1)); r[2] = int16(a + int256(2)); return r; }
    function f5x(int16[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int8 x, int8 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc48000_0 {
    Cee_cc48000_0 c;
    constructor() { c = new Cee_cc48000_0(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint32 x0, uint128 x1, uint64 x2) = c.f2(a0, a1, a2); return int256(uint256(x0)) + int256(uint256(x1)) + int256(uint256(x2)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (int128 x0, int256 x1) = c.f3(a0, a1); return int256(x0) + int256(x1); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { Scc48000_0 memory p = c.f4(a0, a1); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int8(a), int8(b)); }
}
