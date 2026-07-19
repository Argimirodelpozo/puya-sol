// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc24300_7).
struct Scc24300_7 { int16 m0; uint128 m1; }
contract Cee_cc24300_7 {
    function f1(int256 a) external pure returns (int256) { return int256(a); }
    function f2(int256 a0, int256 a1) external pure returns (uint128, uint128) { return (uint128(uint256(a0)), uint128(uint256(a1))); }
    function f3(int256 a0, int256 a1) external pure returns (uint8, int64) { return (uint8(uint256(a0)), int64(a1)); }
    function f4(int256 a0, int256 a1) external pure returns (Scc24300_7 memory) { return Scc24300_7(int16(a0), uint128(uint256(a1))); }
    function f4x(Scc24300_7 memory p) external pure returns (int256) { return int256(p.m0) + int256(uint256(p.m1)); }
    function f5(int256 a) external pure returns (int32[] memory) { int32[] memory r = new int32[](3); r[0] = int32(a + int256(0)); r[1] = int32(a + int256(1)); r[2] = int32(a + int256(2)); return r; }
    function f5x(int32[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int8 x, int128 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc24300_7 {
    Cee_cc24300_7 c;
    constructor() { c = new Cee_cc24300_7(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1) external view returns (int256) { (uint128 x0, uint128 x1) = c.f2(a0, a1); return int256(uint256(x0)) + int256(uint256(x1)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (uint8 x0, int64 x1) = c.f3(a0, a1); return int256(uint256(x0)) + int256(x1); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { Scc24300_7 memory p = c.f4(a0, a1); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int8(a), int128(b)); }
}
