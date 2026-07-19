// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc24300_1).
struct Scc24300_1 { int64 m0; uint8 m1; }
contract Cee_cc24300_1 {
    function f1(int256 a) external pure returns (int16) { return int16(a); }
    function f2(int256 a) external pure returns (int16) { return int16(a); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (uint64, int8, int8) { return (uint64(uint256(a0)), int8(a1), int8(a2)); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (uint128, int64, int16) { return (uint128(uint256(a0)), int64(a1), int16(a2)); }
    function f5(int256 a0, int256 a1) external pure returns (Scc24300_1 memory) { return Scc24300_1(int64(a0), uint8(uint256(a1))); }
    function f5x(Scc24300_1 memory p) external pure returns (int256) { return int256(p.m0) + int256(uint256(p.m1)); }
    function f6(int256 a) external pure returns (int128[] memory) { int128[] memory r = new int128[](3); r[0] = int128(a + int256(0)); r[1] = int128(a + int256(1)); r[2] = int128(a + int256(2)); return r; }
    function f6x(int128[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f7(int16 x, int64 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc24300_1 {
    Cee_cc24300_1 c;
    constructor() { c = new Cee_cc24300_1(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint64 x0, int8 x1, int8 x2) = c.f3(a0, a1, a2); return int256(uint256(x0)) + int256(x1) + int256(x2); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint128 x0, int64 x1, int16 x2) = c.f4(a0, a1, a2); return int256(uint256(x0)) + int256(x1) + int256(x2); }
    function gf5(int256 a0, int256 a1) external view returns (int256) { Scc24300_1 memory p = c.f5(a0, a1); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int16(a), int64(b)); }
}
