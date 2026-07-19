// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc24300_5).
struct Scc24300_5 { int128 m0; uint8 m1; bool m2; }
contract Cee_cc24300_5 {
    function f1(int256 a) external pure returns (int128) { return int128(a); }
    function f2(int256 a0, int256 a1) external pure returns (int32, bytes4) { return (int32(a0), bytes4(uint32(uint256(a1)))); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (int256, uint8, int64) { return (int256(a0), uint8(uint256(a1)), int64(a2)); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (Scc24300_5 memory) { return Scc24300_5(int128(a0), uint8(uint256(a1)), (a2 % 2 == 0)); }
    function f4x(Scc24300_5 memory p) external pure returns (int256) { return int256(p.m0) + int256(uint256(p.m1)) + (p.m2 ? int256(1) : int256(0)); }
    function f5(int256 a) external pure returns (uint16[] memory) { uint16[] memory r = new uint16[](3); r[0] = uint16(uint256(a + int256(0))); r[1] = uint16(uint256(a + int256(1))); r[2] = uint16(uint256(a + int256(2))); return r; }
    function f5x(uint16[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(uint256(xs[i])); return s; }
    function f6(int32 x, int256 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc24300_5 {
    Cee_cc24300_5 c;
    constructor() { c = new Cee_cc24300_5(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1) external view returns (int256) { (int32 x0, bytes4 x1) = c.f2(a0, a1); return int256(x0) + int256(uint256(uint32(x1))); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (int256 x0, uint8 x1, int64 x2) = c.f3(a0, a1, a2); return int256(x0) + int256(uint256(x1)) + int256(x2); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { Scc24300_5 memory p = c.f4(a0, a1, a2); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int32(a), int256(b)); }
}
