// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc48000_8).
struct Scc48000_8 { bytes32 m0; int64 m1; bytes32 m2; }
contract Cee_cc48000_8 {
    function f1(int256 a) external pure returns (int64) { return int64(a); }
    function f2(int256 a0, int256 a1, int256 a2) external pure returns (bytes4, bytes4, int64) { return (bytes4(uint32(uint256(a0))), bytes4(uint32(uint256(a1))), int64(a2)); }
    function f3(int256 a0, int256 a1) external pure returns (uint256, address) { return (uint256(uint256(a0)), address(uint160(uint256(a1)))); }
    function f4(int256 a0, int256 a1, int256 a2) external pure returns (Scc48000_8 memory) { return Scc48000_8(bytes32(uint256(uint256(a0))), int64(a1), bytes32(uint256(uint256(a2)))); }
    function f4x(Scc48000_8 memory p) external pure returns (int256) { return int256(uint256(uint256(p.m0))) + int256(p.m1) + int256(uint256(uint256(p.m2))); }
    function f5(int256 a) external pure returns (int64[] memory) { int64[] memory r = new int64[](2); r[0] = int64(a + int256(0)); r[1] = int64(a + int256(1)); return r; }
    function f5x(int64[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(xs[i]); return s; }
    function f6(int8 x, int128 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc48000_8 {
    Cee_cc48000_8 c;
    constructor() { c = new Cee_cc48000_8(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a0, int256 a1, int256 a2) external view returns (int256) { (bytes4 x0, bytes4 x1, int64 x2) = c.f2(a0, a1, a2); return int256(uint256(uint32(x0))) + int256(uint256(uint32(x1))) + int256(x2); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (uint256 x0, address x1) = c.f3(a0, a1); return int256(uint256(x0)) + int256(uint256(uint160(x1))); }
    function gf4(int256 a0, int256 a1, int256 a2) external view returns (int256) { Scc48000_8 memory p = c.f4(a0, a1, a2); return c.f4x(p); }
    function gf5(int256 a) external view returns (int256) { return c.f5x(c.f5(a)); }
    function gf6(int256 a, int256 b) external view returns (int256) { return c.f6(int8(a), int128(b)); }
}
