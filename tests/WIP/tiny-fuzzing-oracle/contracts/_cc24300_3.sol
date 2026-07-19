// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc24300_3).
struct Scc24300_3 { bool m0; int32 m1; uint32 m2; int8 m3; }
contract Cee_cc24300_3 {
    function f1(int256 a) external pure returns (int8) { return int8(a); }
    function f2(int256 a0, int256 a1) external pure returns (int256, address) { return (int256(a0), address(uint160(uint256(a1)))); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (bytes32, int32, address) { return (bytes32(uint256(uint256(a0))), int32(a1), address(uint160(uint256(a2)))); }
    function f4(int256 a0, int256 a1, int256 a2, int256 a3) external pure returns (Scc24300_3 memory) { return Scc24300_3((a0 % 2 == 0), int32(a1), uint32(uint256(a2)), int8(a3)); }
    function f4x(Scc24300_3 memory p) external pure returns (int256) { return (p.m0 ? int256(1) : int256(0)) + int256(p.m1) + int256(uint256(p.m2)) + int256(p.m3); }
    function f5(int256 a) external pure returns (int8[] memory) { int8[] memory r = new int8[](2); r[0] = int8(a + int256(0)); r[1] = int8(a + int256(1)); return r; }
    function f5x(int8[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int8 x, int64 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc24300_3 {
    Cee_cc24300_3 c;
    constructor() { c = new Cee_cc24300_3(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1) external view returns (int256) { (int256 x0, address x1) = c.f2(a0, a1); return int256(x0) + int256(uint256(uint160(x1))); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (bytes32 x0, int32 x1, address x2) = c.f3(a0, a1, a2); return int256(uint256(uint256(x0))) + int256(x1) + int256(uint256(uint160(x2))); }
    function gf4(int256 a0, int256 a1, int256 a2, int256 a3) external view returns (int256) { Scc24300_3 memory p = c.f4(a0, a1, a2, a3); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int8(a), int64(b)); }
}
