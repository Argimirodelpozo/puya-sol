// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc700_0).
struct Scc700_0 { bytes4 m0; int256 m1; }
contract Cee_cc700_0 {
    function f1(int256 a) external pure returns (int256) { return int256(a); }
    function f2(int256 a) external pure returns (int16) { return int16(a); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (uint8, int64, int32) { return (uint8(uint256(a0)), int64(a1), int32(a2)); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (bytes4, int256, int256) { return (bytes4(uint32(uint256(a0))), int256(a1), int256(a2)); }
    function f5(int256 a0, int256 a1) external pure returns (Scc700_0 memory) { return Scc700_0(bytes4(uint32(uint256(a0))), int256(a1)); }
    function f5x(Scc700_0 memory p) external pure returns (int256) { return int256(uint256(uint32(p.m0))) + int256(p.m1); }
    function f6(int256 a) external pure returns (uint16[] memory) { uint16[] memory r = new uint16[](2); r[0] = uint16(uint256(a + int256(0))); r[1] = uint16(uint256(a + int256(1))); return r; }
    function f6x(uint16[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(uint256(xs[i])); return s; }
    function f7(int64 x, int256 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc700_0 {
    Cee_cc700_0 c;
    constructor() { c = new Cee_cc700_0(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint8 x0, int64 x1, int32 x2) = c.f3(a0, a1, a2); return int256(uint256(x0)) + int256(x1) + int256(x2); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { (bytes4 x0, int256 x1, int256 x2) = c.f4(a0, a1, a2); return int256(uint256(uint32(x0))) + int256(x1) + int256(x2); }
    function gf5(int256 a0, int256 a1) external view returns (int256) { Scc700_0 memory p = c.f5(a0, a1); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int64(a), int256(b)); }
}
