// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc1_7).
struct Scc1_7 { uint256 m0; int64 m1; }
contract Cee_cc1_7 {
    function f1(int256 a) external pure returns (int128) { return int128(a); }
    function f2(int256 a0, int256 a1) external pure returns (address, int128) { return (address(uint160(uint256(a0))), int128(a1)); }
    function f3(int256 a0, int256 a1) external pure returns (int64, uint64) { return (int64(a0), uint64(uint256(a1))); }
    function f4(int256 a0, int256 a1) external pure returns (Scc1_7 memory) { return Scc1_7(uint256(uint256(a0)), int64(a1)); }
    function f4x(Scc1_7 memory p) external pure returns (int256) { return int256(uint256(p.m0)) + int256(p.m1); }
    function f5(int256 a) external pure returns (int128[] memory) { int128[] memory r = new int128[](2); r[0] = int128(a + int256(0)); r[1] = int128(a + int256(1)); return r; }
    function f5x(int128[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int64 x, int128 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc1_7 {
    Cee_cc1_7 c;
    constructor() { c = new Cee_cc1_7(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1) external view returns (int256) { (address x0, int128 x1) = c.f2(a0, a1); return int256(uint256(uint160(x0))) + int256(x1); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (int64 x0, uint64 x1) = c.f3(a0, a1); return int256(x0) + int256(uint256(x1)); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { Scc1_7 memory p = c.f4(a0, a1); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int64(a), int128(b)); }
}
