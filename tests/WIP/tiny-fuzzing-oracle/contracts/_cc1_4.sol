// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc1_4).
struct Scc1_4 { bool m0; bytes4 m1; }
contract Cee_cc1_4 {
    function f1(int256 a) external pure returns (int8) { return int8(a); }
    function f2(int256 a) external pure returns (int256) { return int256(a); }
    function f3(int256 a0, int256 a1) external pure returns (bytes4, bytes32) { return (bytes4(uint32(uint256(a0))), bytes32(uint256(uint256(a1)))); }
    function f4(int256 a0, int256 a1) external pure returns (int256, uint256) { return (int256(a0), uint256(uint256(a1))); }
    function f5(int256 a0, int256 a1) external pure returns (Scc1_4 memory) { return Scc1_4((a0 % 2 == 0), bytes4(uint32(uint256(a1)))); }
    function f5x(Scc1_4 memory p) external pure returns (int256) { return (p.m0 ? int256(1) : int256(0)) + int256(uint256(uint32(p.m1))); }
    function f6(int256 a) external pure returns (uint16[] memory) { uint16[] memory r = new uint16[](2); r[0] = uint16(uint256(a + int256(0))); r[1] = uint16(uint256(a + int256(1))); return r; }
    function f6x(uint16[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(uint256(xs[i])); return s; }
    function f7(int32 x, int8 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc1_4 {
    Cee_cc1_4 c;
    constructor() { c = new Cee_cc1_4(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (bytes4 x0, bytes32 x1) = c.f3(a0, a1); return int256(uint256(uint32(x0))) + int256(uint256(uint256(x1))); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { (int256 x0, uint256 x1) = c.f4(a0, a1); return int256(x0) + int256(uint256(x1)); }
    function gf5(int256 a0, int256 a1) external view returns (int256) { Scc1_4 memory p = c.f5(a0, a1); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int32(a), int8(b)); }
}
