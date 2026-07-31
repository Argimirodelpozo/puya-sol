// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc700_4).
struct Scc700_4 { int256 m0; uint128 m1; }
contract Cee_cc700_4 {
    function f1(int256 a) external pure returns (int16) { return int16(a); }
    function f2(int256 a0, int256 a1, int256 a2) external pure returns (uint128, uint128, uint32) { return (uint128(uint256(a0)), uint128(uint256(a1)), uint32(uint256(a2))); }
    function f3(int256 a0, int256 a1) external pure returns (uint64, int8) { return (uint64(uint256(a0)), int8(a1)); }
    function f4(int256 a0, int256 a1) external pure returns (Scc700_4 memory) { return Scc700_4(int256(a0), uint128(uint256(a1))); }
    function f4x(Scc700_4 memory p) external pure returns (int256) { return int256(p.m0) + int256(uint256(p.m1)); }
    function f5(int256 a) external pure returns (int8[] memory) { int8[] memory r = new int8[](3); r[0] = int8(a + int256(0)); r[1] = int8(a + int256(1)); r[2] = int8(a + int256(2)); return r; }
    function f5x(int8[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int16 x, int128 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc700_4 {
    Cee_cc700_4 c;
    constructor() { c = new Cee_cc700_4(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint128 x0, uint128 x1, uint32 x2) = c.f2(a0, a1, a2); return int256(uint256(x0)) + int256(uint256(x1)) + int256(uint256(x2)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (uint64 x0, int8 x1) = c.f3(a0, a1); return int256(uint256(x0)) + int256(x1); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { Scc700_4 memory p = c.f4(a0, a1); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int16(a), int128(b)); }
}
