// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc700_9).
struct Scc700_9 { bool m0; uint128 m1; bytes32 m2; }
contract Cee_cc700_9 {
    function f1(int256 a) external pure returns (int128) { return int128(a); }
    function f2(int256 a0, int256 a1) external pure returns (int256, uint128) { return (int256(a0), uint128(uint256(a1))); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (bytes4, uint64, int8) { return (bytes4(uint32(uint256(a0))), uint64(uint256(a1)), int8(a2)); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (Scc700_9 memory) { return Scc700_9((a0 % 2 == 0), uint128(uint256(a1)), bytes32(uint256(uint256(a2)))); }
    function f4x(Scc700_9 memory p) external pure returns (int256) { return (p.m0 ? int256(1) : int256(0)) + int256(uint256(p.m1)) + int256(uint256(uint256(p.m2))); }
    function f5(int256 a) external pure returns (int32[] memory) { int32[] memory r = new int32[](3); r[0] = int32(a + int256(0)); r[1] = int32(a + int256(1)); r[2] = int32(a + int256(2)); return r; }
    function f5x(int32[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int8 x, int8 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc700_9 {
    Cee_cc700_9 c;
    constructor() { c = new Cee_cc700_9(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1) external view returns (int256) { (int256 x0, uint128 x1) = c.f2(a0, a1); return int256(x0) + int256(uint256(x1)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (bytes4 x0, uint64 x1, int8 x2) = c.f3(a0, a1, a2); return int256(uint256(uint32(x0))) + int256(uint256(x1)) + int256(x2); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { Scc700_9 memory p = c.f4(a0, a1, a2); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int8(a), int8(b)); }
}
