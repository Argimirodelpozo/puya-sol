// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc700_6).
struct Scc700_6 { uint128 m0; uint128 m1; }
contract Cee_cc700_6 {
    function f1(int256 a) external pure returns (int64) { return int64(a); }
    function f2(int256 a0, int256 a1, int256 a2) external pure returns (uint8, uint64, int16) { return (uint8(uint256(a0)), uint64(uint256(a1)), int16(a2)); }
    function f3(int256 a0, int256 a1) external pure returns (int32, int128) { return (int32(a0), int128(a1)); }
    function f4(int256 a0, int256 a1) external pure returns (Scc700_6 memory) { return Scc700_6(uint128(uint256(a0)), uint128(uint256(a1))); }
    function f4x(Scc700_6 memory p) external pure returns (int256) { return int256(uint256(p.m0)) + int256(uint256(p.m1)); }
    function f5(int256 a) external pure returns (int32[] memory) { int32[] memory r = new int32[](3); r[0] = int32(a + int256(0)); r[1] = int32(a + int256(1)); r[2] = int32(a + int256(2)); return r; }
    function f5x(int32[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int64 x, int64 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc700_6 {
    Cee_cc700_6 c;
    constructor() { c = new Cee_cc700_6(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint8 x0, uint64 x1, int16 x2) = c.f2(a0, a1, a2); return int256(uint256(x0)) + int256(uint256(x1)) + int256(x2); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (int32 x0, int128 x1) = c.f3(a0, a1); return int256(x0) + int256(x1); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { Scc700_6 memory p = c.f4(a0, a1); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int64(a), int64(b)); }
}
