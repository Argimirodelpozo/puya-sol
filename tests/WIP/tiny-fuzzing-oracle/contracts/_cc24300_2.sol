// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc24300_2).
struct Scc24300_2 { uint32 m0; bytes4 m1; int128 m2; int8 m3; }
contract Cee_cc24300_2 {
    function f1(int256 a) external pure returns (int128) { return int128(a); }
    function f2(int256 a0, int256 a1) external pure returns (uint8, uint128) { return (uint8(uint256(a0)), uint128(uint256(a1))); }
    function f3(int256 a0, int256 a1) external pure returns (bytes4, uint8) { return (bytes4(uint32(uint256(a0))), uint8(uint256(a1))); }
    function f4(int256 a0, int256 a1, int256 a2, int256 a3) external pure returns (Scc24300_2 memory) { return Scc24300_2(uint32(uint256(a0)), bytes4(uint32(uint256(a1))), int128(a2), int8(a3)); }
    function f4x(Scc24300_2 memory p) external pure returns (int256) { return int256(uint256(p.m0)) + int256(uint256(uint32(p.m1))) + int256(p.m2) + int256(p.m3); }
    function f5(int256 a) external pure returns (uint128[] memory) { uint128[] memory r = new uint128[](2); r[0] = uint128(uint256(a + int256(0))); r[1] = uint128(uint256(a + int256(1))); return r; }
    function f5x(uint128[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(uint256(xs[i])); return s; }
    function f6(int64 x, int128 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc24300_2 {
    Cee_cc24300_2 c;
    constructor() { c = new Cee_cc24300_2(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1) external view returns (int256) { (uint8 x0, uint128 x1) = c.f2(a0, a1); return int256(uint256(x0)) + int256(uint256(x1)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (bytes4 x0, uint8 x1) = c.f3(a0, a1); return int256(uint256(uint32(x0))) + int256(uint256(x1)); }
    function gf4(int256 a0, int256 a1, int256 a2, int256 a3) external view returns (int256) { Scc24300_2 memory p = c.f4(a0, a1, a2, a3); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int64(a), int128(b)); }
}
