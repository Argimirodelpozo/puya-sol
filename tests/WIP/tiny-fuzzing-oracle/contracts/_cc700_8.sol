// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc700_8).
struct Scc700_8 { uint8 m0; bytes32 m1; }
contract Cee_cc700_8 {
    function f1(int256 a) external pure returns (int16) { return int16(a); }
    function f2(int256 a) external pure returns (int256) { return int256(a); }
    function f3(int256 a0, int256 a1) external pure returns (uint128, bytes4) { return (uint128(uint256(a0)), bytes4(uint32(uint256(a1)))); }
    function f4(int256 a0, int256 a1) external pure returns (int8, int128) { return (int8(a0), int128(a1)); }
    function f5(int256 a0, int256 a1) external pure returns (Scc700_8 memory) { return Scc700_8(uint8(uint256(a0)), bytes32(uint256(uint256(a1)))); }
    function f5x(Scc700_8 memory p) external pure returns (int256) { return int256(uint256(p.m0)) + int256(uint256(uint256(p.m1))); }
    function f6(int256 a) external pure returns (int16[] memory) { int16[] memory r = new int16[](2); r[0] = int16(a + int256(0)); r[1] = int16(a + int256(1)); return r; }
    function f6x(int16[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f7(int16 x, int64 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc700_8 {
    Cee_cc700_8 c;
    constructor() { c = new Cee_cc700_8(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (uint128 x0, bytes4 x1) = c.f3(a0, a1); return int256(uint256(x0)) + int256(uint256(uint32(x1))); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { (int8 x0, int128 x1) = c.f4(a0, a1); return int256(x0) + int256(x1); }
    function gf5(int256 a0, int256 a1) external view returns (int256) { Scc700_8 memory p = c.f5(a0, a1); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int16(a), int64(b)); }
}
