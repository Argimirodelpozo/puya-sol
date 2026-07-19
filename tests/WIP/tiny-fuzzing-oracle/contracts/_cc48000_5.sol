// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc48000_5).
struct Scc48000_5 { uint64 m0; int32 m1; }
contract Cee_cc48000_5 {
    function f1(int256 a) external pure returns (int8) { return int8(a); }
    function f2(int256 a0, int256 a1, int256 a2) external pure returns (address, bytes32, int16) { return (address(uint160(uint256(a0))), bytes32(uint256(uint256(a1))), int16(a2)); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (uint8, int16, bytes4) { return (uint8(uint256(a0)), int16(a1), bytes4(uint32(uint256(a2)))); }
    function f4(int256 a0, int256 a1) external pure returns (Scc48000_5 memory) { return Scc48000_5(uint64(uint256(a0)), int32(a1)); }
    function f4x(Scc48000_5 memory p) external pure returns (int256) { return int256(uint256(p.m0)) + int256(p.m1); }
    function f5(int256 a) external pure returns (int8[] memory) { int8[] memory r = new int8[](2); r[0] = int8(a + int256(0)); r[1] = int8(a + int256(1)); return r; }
    function f5x(int8[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int16 x, int64 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc48000_5 {
    Cee_cc48000_5 c;
    constructor() { c = new Cee_cc48000_5(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1, int256 a2) external view returns (int256) { (address x0, bytes32 x1, int16 x2) = c.f2(a0, a1, a2); return int256(uint256(uint160(x0))) + int256(uint256(uint256(x1))) + int256(x2); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint8 x0, int16 x1, bytes4 x2) = c.f3(a0, a1, a2); return int256(uint256(x0)) + int256(x1) + int256(uint256(uint32(x2))); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { Scc48000_5 memory p = c.f4(a0, a1); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int16(a), int64(b)); }
}
