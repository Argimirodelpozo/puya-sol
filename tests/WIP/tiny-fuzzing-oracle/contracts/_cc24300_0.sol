// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc24300_0).
struct Scc24300_0 { int32 m0; int128 m1; int8 m2; int64 m3; }
contract Cee_cc24300_0 {
    function f1(int256 a) external pure returns (int32) { return int32(a); }
    function f2(int256 a) external pure returns (int64) { return int64(a); }
    function f3(int256 a0, int256 a1) external pure returns (bool, uint256) { return ((a0 % 2 == 0), uint256(uint256(a1))); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (bytes32, address, int64) { return (bytes32(uint256(uint256(a0))), address(uint160(uint256(a1))), int64(a2)); }
    function f5(int256 a0, int256 a1, int256 a2, int256 a3) external pure returns (Scc24300_0 memory) { return Scc24300_0(int32(a0), int128(a1), int8(a2), int64(a3)); }
    function f5x(Scc24300_0 memory p) external pure returns (int256) { return int256(p.m0) + int256(p.m1) + int256(p.m2) + int256(p.m3); }
    function f6(int256 a) external pure returns (int128[] memory) { int128[] memory r = new int128[](2); r[0] = int128(a + int256(0)); r[1] = int128(a + int256(1)); return r; }
    function f6x(int128[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f7(int8 x, int256 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc24300_0 {
    Cee_cc24300_0 c;
    constructor() { c = new Cee_cc24300_0(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (bool x0, uint256 x1) = c.f3(a0, a1); return (x0 ? int256(1) : int256(0)) + int256(uint256(x1)); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { (bytes32 x0, address x1, int64 x2) = c.f4(a0, a1, a2); return int256(uint256(uint256(x0))) + int256(uint256(uint160(x1))) + int256(x2); }
    function gf5(int256 a0, int256 a1, int256 a2, int256 a3) external view returns (int256) { Scc24300_0 memory p = c.f5(a0, a1, a2, a3); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int8(a), int256(b)); }
}
