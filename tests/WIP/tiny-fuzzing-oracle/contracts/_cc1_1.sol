// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc1_1).
struct Scc1_1 { int256 m0; uint128 m1; uint256 m2; bytes4 m3; }
contract Cee_cc1_1 {
    function f1(int256 a) external pure returns (int16) { return int16(a); }
    function f2(int256 a0, int256 a1) external pure returns (uint64, address) { return (uint64(uint256(a0)), address(uint160(uint256(a1)))); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (bytes4, int32, uint8) { return (bytes4(uint32(uint256(a0))), int32(a1), uint8(uint256(a2))); }
    function f4(int256 a0, int256 a1, int256 a2, int256 a3) external pure returns (Scc1_1 memory) { return Scc1_1(int256(a0), uint128(uint256(a1)), uint256(uint256(a2)), bytes4(uint32(uint256(a3)))); }
    function f4x(Scc1_1 memory p) external pure returns (int256) { return int256(p.m0) + int256(uint256(p.m1)) + int256(uint256(p.m2)) + int256(uint256(uint32(p.m3))); }
    function f5(int256 a) external pure returns (int64[] memory) { int64[] memory r = new int64[](2); r[0] = int64(a + int256(0)); r[1] = int64(a + int256(1)); return r; }
    function f5x(int64[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int8 x, int8 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc1_1 {
    Cee_cc1_1 c;
    constructor() { c = new Cee_cc1_1(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1) external view returns (int256) { (uint64 x0, address x1) = c.f2(a0, a1); return int256(uint256(x0)) + int256(uint256(uint160(x1))); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (bytes4 x0, int32 x1, uint8 x2) = c.f3(a0, a1, a2); return int256(uint256(uint32(x0))) + int256(x1) + int256(uint256(x2)); }
    function gf4(int256 a0, int256 a1, int256 a2, int256 a3) external view returns (int256) { Scc1_1 memory p = c.f4(a0, a1, a2, a3); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int8(a), int8(b)); }
}
