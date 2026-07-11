// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc1_2).
struct Scc1_2 { int16 m0; bool m1; uint8 m2; uint64 m3; }
contract Cee_cc1_2 {
    function f1(int256 a) external pure returns (int16) { return int16(a); }
    function f2(int256 a) external pure returns (int16) { return int16(a); }
    function f3(int256 a0, int256 a1) external pure returns (int128, uint32) { return (int128(a0), uint32(uint256(a1))); }
    function f4(int256 a0, int256 a1) external pure returns (uint256, uint128) { return (uint256(uint256(a0)), uint128(uint256(a1))); }
    function f5(int256 a0, int256 a1, int256 a2, int256 a3) external pure returns (Scc1_2 memory) { return Scc1_2(int16(a0), (a1 % 2 == 0), uint8(uint256(a2)), uint64(uint256(a3))); }
    function f5x(Scc1_2 memory p) external pure returns (int256) { return int256(p.m0) + (p.m1 ? int256(1) : int256(0)) + int256(uint256(p.m2)) + int256(uint256(p.m3)); }
    function f6(int256 a) external pure returns (int8[] memory) { int8[] memory r = new int8[](2); r[0] = int8(a + int256(0)); r[1] = int8(a + int256(1)); return r; }
    function f6x(int8[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f7(int16 x, int8 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc1_2 {
    Cee_cc1_2 c;
    constructor() { c = new Cee_cc1_2(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (int128 x0, uint32 x1) = c.f3(a0, a1); return int256(x0) + int256(uint256(x1)); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { (uint256 x0, uint128 x1) = c.f4(a0, a1); return int256(uint256(x0)) + int256(uint256(x1)); }
    function gf5(int256 a0, int256 a1, int256 a2, int256 a3) external view returns (int256) { Scc1_2 memory p = c.f5(a0, a1, a2, a3); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int16(a), int8(b)); }
}
