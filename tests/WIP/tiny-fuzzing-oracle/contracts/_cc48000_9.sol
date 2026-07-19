// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc48000_9).
struct Scc48000_9 { int32 m0; address m1; bool m2; }
contract Cee_cc48000_9 {
    function f1(int256 a) external pure returns (int16) { return int16(a); }
    function f2(int256 a) external pure returns (int64) { return int64(a); }
    function f3(int256 a0, int256 a1, int256 a2) external pure returns (int128, uint256, int256) { return (int128(a0), uint256(uint256(a1)), int256(a2)); }
    function f4(int256 a0, int256 a1) external pure returns (int16, address) { return (int16(a0), address(uint160(uint256(a1)))); }
    function f5(int256 a0, int256 a1, int256 a2) external pure returns (Scc48000_9 memory) { return Scc48000_9(int32(a0), address(uint160(uint256(a1))), (a2 % 2 == 0)); }
    function f5x(Scc48000_9 memory p) external pure returns (int256) { return int256(p.m0) + int256(uint256(uint160(p.m1))) + (p.m2 ? int256(1) : int256(0)); }
    function f6(int256 a) external pure returns (uint128[] memory) { uint128[] memory r = new uint128[](2); r[0] = uint128(uint256(a + int256(0))); r[1] = uint128(uint256(a + int256(1))); return r; }
    function f6x(uint128[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(uint256(xs[i])); return s; }
    function f7(int64 x, int256 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc48000_9 {
    Cee_cc48000_9 c;
    constructor() { c = new Cee_cc48000_9(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1, int256 a2) external view returns (int256) { (int128 x0, uint256 x1, int256 x2) = c.f3(a0, a1, a2); return int256(x0) + int256(uint256(x1)) + int256(x2); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { (int16 x0, address x1) = c.f4(a0, a1); return int256(x0) + int256(uint256(uint160(x1))); }
    function gf5(int256 a0, int256 a1, int256 a2) external view returns (int256) { Scc48000_9 memory p = c.f5(a0, a1, a2); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int64(a), int256(b)); }
}
