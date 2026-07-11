// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc1_3).
struct Scc1_3 { bool m0; int256 m1; }
contract Cee_cc1_3 {
    function f1(int256 a) external pure returns (int128) { return int128(a); }
    function f2(int256 a) external pure returns (int32) { return int32(a); }
    function f3(int256 a0, int256 a1) external pure returns (bool, uint32) { return ((a0 % 2 == 0), uint32(uint256(a1))); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (uint64, uint32, uint8) { return (uint64(uint256(a0)), uint32(uint256(a1)), uint8(uint256(a2))); }
    function f5(int256 a0, int256 a1) external pure returns (Scc1_3 memory) { return Scc1_3((a0 % 2 == 0), int256(a1)); }
    function f5x(Scc1_3 memory p) external pure returns (int256) { return (p.m0 ? int256(1) : int256(0)) + int256(p.m1); }
    function f6(int256 a) external pure returns (int8[] memory) { int8[] memory r = new int8[](3); r[0] = int8(a + int256(0)); r[1] = int8(a + int256(1)); r[2] = int8(a + int256(2)); return r; }
    function f6x(int8[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f7(int64 x, int128 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc1_3 {
    Cee_cc1_3 c;
    constructor() { c = new Cee_cc1_3(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (bool x0, uint32 x1) = c.f3(a0, a1); return (x0 ? int256(1) : int256(0)) + int256(uint256(x1)); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint64 x0, uint32 x1, uint8 x2) = c.f4(a0, a1, a2); return int256(uint256(x0)) + int256(uint256(x1)) + int256(uint256(x2)); }
    function gf5(int256 a0, int256 a1) external view returns (int256) { Scc1_3 memory p = c.f5(a0, a1); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int64(a), int128(b)); }
}
