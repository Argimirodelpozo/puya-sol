// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc48000_6).
struct Scc48000_6 { int128 m0; uint256 m1; uint32 m2; bool m3; }
contract Cee_cc48000_6 {
    function f1(int256 a) external pure returns (int256) { return int256(a); }
    function f2(int256 a) external pure returns (int8) { return int8(a); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (bytes32, int256, int256) { return (bytes32(uint256(uint256(a0))), int256(a1), int256(a2)); }
    function f4(int256 a0, int256 a1) external pure returns (address, bytes4) { return (address(uint160(uint256(a0))), bytes4(uint32(uint256(a1)))); }
    function f5(int256 a0, int256 a1, int256 a2, int256 a3) external pure returns (Scc48000_6 memory) { return Scc48000_6(int128(a0), uint256(uint256(a1)), uint32(uint256(a2)), (a3 % 2 == 0)); }
    function f5x(Scc48000_6 memory p) external pure returns (int256) { return int256(p.m0) + int256(uint256(p.m1)) + int256(uint256(p.m2)) + (p.m3 ? int256(1) : int256(0)); }
    function f6(int256 a) external pure returns (int32[] memory) { int32[] memory r = new int32[](3); r[0] = int32(a + int256(0)); r[1] = int32(a + int256(1)); r[2] = int32(a + int256(2)); return r; }
    function f6x(int32[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f7(int16 x, int128 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc48000_6 {
    Cee_cc48000_6 c;
    constructor() { c = new Cee_cc48000_6(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (bytes32 x0, int256 x1, int256 x2) = c.f3(a0, a1, a2); return int256(uint256(uint256(x0))) + int256(x1) + int256(x2); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { (address x0, bytes4 x1) = c.f4(a0, a1); return int256(uint256(uint160(x0))) + int256(uint256(uint32(x1))); }
    function gf5(int256 a0, int256 a1, int256 a2, int256 a3) external view returns (int256) { Scc48000_6 memory p = c.f5(a0, a1, a2, a3); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int16(a), int128(b)); }
}
