// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc48000_4).
struct Scc48000_4 { uint8 m0; int128 m1; uint128 m2; bytes32 m3; }
contract Cee_cc48000_4 {
    function f1(int256 a) external pure returns (int16) { return int16(a); }
    function f2(int256 a0, int256 a1) external pure returns (int8, int16) { return (int8(a0), int16(a1)); }
    function f3(int256 a0, int256 a1) external pure returns (uint64, address) { return (uint64(uint256(a0)), address(uint160(uint256(a1)))); }
    function f4(int256 a0, int256 a1, int256 a2, int256 a3) external pure returns (Scc48000_4 memory) { return Scc48000_4(uint8(uint256(a0)), int128(a1), uint128(uint256(a2)), bytes32(uint256(uint256(a3)))); }
    function f4x(Scc48000_4 memory p) external pure returns (int256) { return int256(uint256(p.m0)) + int256(p.m1) + int256(uint256(p.m2)) + int256(uint256(uint256(p.m3))); }
    function f5(int256 a) external pure returns (uint128[] memory) { uint128[] memory r = new uint128[](3); r[0] = uint128(uint256(a + int256(0))); r[1] = uint128(uint256(a + int256(1))); r[2] = uint128(uint256(a + int256(2))); return r; }
    function f5x(uint128[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(uint256(xs[i])); return s; }
    function f6(int32 x, int256 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc48000_4 {
    Cee_cc48000_4 c;
    constructor() { c = new Cee_cc48000_4(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1) external view returns (int256) { (int8 x0, int16 x1) = c.f2(a0, a1); return int256(x0) + int256(x1); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (uint64 x0, address x1) = c.f3(a0, a1); return int256(uint256(x0)) + int256(uint256(uint160(x1))); }
    function gf4(int256 a0, int256 a1, int256 a2, int256 a3) external view returns (int256) { Scc48000_4 memory p = c.f4(a0, a1, a2, a3); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int32(a), int256(b)); }
}
