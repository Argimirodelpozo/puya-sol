// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc48000_7).
struct Scc48000_7 { uint128 m0; int8 m1; }
contract Cee_cc48000_7 {
    function f1(int256 a) external pure returns (int16) { return int16(a); }
    function f2(int256 a0, int256 a1) external pure returns (int256, uint64) { return (int256(a0), uint64(uint256(a1))); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (bytes32, uint256, int64) { return (bytes32(uint256(uint256(a0))), uint256(uint256(a1)), int64(a2)); }
    function f4(int256 a0, int256 a1) external pure returns (Scc48000_7 memory) { return Scc48000_7(uint128(uint256(a0)), int8(a1)); }
    function f4x(Scc48000_7 memory p) external pure returns (int256) { return int256(uint256(p.m0)) + int256(p.m1); }
    function f5(int256 a) external pure returns (int128[] memory) { int128[] memory r = new int128[](2); r[0] = int128(a + int256(0)); r[1] = int128(a + int256(1)); return r; }
    function f5x(int128[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int8 x, int256 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc48000_7 {
    Cee_cc48000_7 c;
    constructor() { c = new Cee_cc48000_7(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1) external view returns (int256) { (int256 x0, uint64 x1) = c.f2(a0, a1); return int256(x0) + int256(uint256(x1)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (bytes32 x0, uint256 x1, int64 x2) = c.f3(a0, a1, a2); return int256(uint256(uint256(x0))) + int256(uint256(x1)) + int256(x2); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { Scc48000_7 memory p = c.f4(a0, a1); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int8(a), int256(b)); }
}
