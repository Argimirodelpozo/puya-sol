// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc1_5).
struct Scc1_5 { uint32 m0; int16 m1; }
contract Cee_cc1_5 {
    function f1(int256 a) external pure returns (int64) { return int64(a); }
    function f2(int256 a) external pure returns (int128) { return int128(a); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (address, uint128, uint256) { return (address(uint160(uint256(a0))), uint128(uint256(a1)), uint256(uint256(a2))); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (int256, uint64, uint256) { return (int256(a0), uint64(uint256(a1)), uint256(uint256(a2))); }
    function f5(int256 a0, int256 a1) external pure returns (Scc1_5 memory) { return Scc1_5(uint32(uint256(a0)), int16(a1)); }
    function f5x(Scc1_5 memory p) external pure returns (int256) { return int256(uint256(p.m0)) + int256(p.m1); }
    function f6(int256 a) external pure returns (int128[] memory) { int128[] memory r = new int128[](3); r[0] = int128(a + int256(0)); r[1] = int128(a + int256(1)); r[2] = int128(a + int256(2)); return r; }
    function f6x(int128[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f7(int32 x, int16 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc1_5 {
    Cee_cc1_5 c;
    constructor() { c = new Cee_cc1_5(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (address x0, uint128 x1, uint256 x2) = c.f3(a0, a1, a2); return int256(uint256(uint160(x0))) + int256(uint256(x1)) + int256(uint256(x2)); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { (int256 x0, uint64 x1, uint256 x2) = c.f4(a0, a1, a2); return int256(x0) + int256(uint256(x1)) + int256(uint256(x2)); }
    function gf5(int256 a0, int256 a1) external view returns (int256) { Scc1_5 memory p = c.f5(a0, a1); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int32(a), int16(b)); }
}
