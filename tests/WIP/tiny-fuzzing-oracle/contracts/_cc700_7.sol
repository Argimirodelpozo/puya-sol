// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc700_7).
struct Scc700_7 { int256 m0; int32 m1; int16 m2; bytes4 m3; }
contract Cee_cc700_7 {
    function f1(int256 a) external pure returns (int128) { return int128(a); }
    function f2(int256 a) external pure returns (int16) { return int16(a); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (address, uint128, int64) { return (address(uint160(uint256(a0))), uint128(uint256(a1)), int64(a2)); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (int16, uint256, int32) { return (int16(a0), uint256(uint256(a1)), int32(a2)); }
    function f5(int256 a0, int256 a1, int256 a2, int256 a3) external pure returns (Scc700_7 memory) { return Scc700_7(int256(a0), int32(a1), int16(a2), bytes4(uint32(uint256(a3)))); }
    function f5x(Scc700_7 memory p) external pure returns (int256) { return int256(p.m0) + int256(p.m1) + int256(p.m2) + int256(uint256(uint32(p.m3))); }
    function f6(int256 a) external pure returns (int16[] memory) { int16[] memory r = new int16[](3); r[0] = int16(a + int256(0)); r[1] = int16(a + int256(1)); r[2] = int16(a + int256(2)); return r; }
    function f6x(int16[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f7(int16 x, int8 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc700_7 {
    Cee_cc700_7 c;
    constructor() { c = new Cee_cc700_7(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (address x0, uint128 x1, int64 x2) = c.f3(a0, a1, a2); return int256(uint256(uint160(x0))) + int256(uint256(x1)) + int256(x2); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { (int16 x0, uint256 x1, int32 x2) = c.f4(a0, a1, a2); return int256(x0) + int256(uint256(x1)) + int256(x2); }
    function gf5(int256 a0, int256 a1, int256 a2, int256 a3) external view returns (int256) { Scc700_7 memory p = c.f5(a0, a1, a2, a3); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int16(a), int8(b)); }
}
