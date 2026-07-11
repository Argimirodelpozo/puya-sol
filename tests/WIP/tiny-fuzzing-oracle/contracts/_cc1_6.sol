// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc1_6).
struct Scc1_6 { address m0; uint64 m1; }
contract Cee_cc1_6 {
    function f1(int256 a) external pure returns (int8) { return int8(a); }
    function f2(int256 a) external pure returns (int16) { return int16(a); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (int8, int64, address) { return (int8(a0), int64(a1), address(uint160(uint256(a2)))); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (uint256, int8, bytes4) { return (uint256(uint256(a0)), int8(a1), bytes4(uint32(uint256(a2)))); }
    function f5(int256 a0, int256 a1) external pure returns (Scc1_6 memory) { return Scc1_6(address(uint160(uint256(a0))), uint64(uint256(a1))); }
    function f5x(Scc1_6 memory p) external pure returns (int256) { return int256(uint256(uint160(p.m0))) + int256(uint256(p.m1)); }
    function f6(int256 a) external pure returns (uint128[] memory) { uint128[] memory r = new uint128[](3); r[0] = uint128(uint256(a + int256(0))); r[1] = uint128(uint256(a + int256(1))); r[2] = uint128(uint256(a + int256(2))); return r; }
    function f6x(uint128[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(uint256(xs[i])); return s; }
    function f7(int16 x, int128 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc1_6 {
    Cee_cc1_6 c;
    constructor() { c = new Cee_cc1_6(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (int8 x0, int64 x1, address x2) = c.f3(a0, a1, a2); return int256(x0) + int256(x1) + int256(uint256(uint160(x2))); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { (uint256 x0, int8 x1, bytes4 x2) = c.f4(a0, a1, a2); return int256(uint256(x0)) + int256(x1) + int256(uint256(uint32(x2))); }
    function gf5(int256 a0, int256 a1) external view returns (int256) { Scc1_6 memory p = c.f5(a0, a1); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int16(a), int128(b)); }
}
