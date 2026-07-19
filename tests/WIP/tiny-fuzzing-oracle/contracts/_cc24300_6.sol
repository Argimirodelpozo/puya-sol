// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED cross-contract differential fixture (fuzz_crosscall.py, tag cc24300_6).
struct Scc24300_6 { address m0; bytes32 m1; }
contract Cee_cc24300_6 {
    function f1(int256 a) external pure returns (int16) { return int16(a); }
    function f2(int256 a) external pure returns (int16) { return int16(a); }
    function f3(int256 a0, int256 a1) external pure returns (bool, bytes4) { return ((a0 % 2 == 0), bytes4(uint32(uint256(a1)))); }
    function f4(int256 a0, int256 a1) external pure returns (int64, int64) { return (int64(a0), int64(a1)); }
    function f5(int256 a0, int256 a1) external pure returns (Scc24300_6 memory) { return Scc24300_6(address(uint160(uint256(a0))), bytes32(uint256(uint256(a1)))); }
    function f5x(Scc24300_6 memory p) external pure returns (int256) { return int256(uint256(uint160(p.m0))) + int256(uint256(uint256(p.m1))); }
    function f6(int256 a) external pure returns (uint16[] memory) { uint16[] memory r = new uint16[](3); r[0] = uint16(uint256(a + int256(0))); r[1] = uint16(uint256(a + int256(1))); r[2] = uint16(uint256(a + int256(2))); return r; }
    function f6x(uint16[] memory xs) external pure returns (int256) { int256 s; for (uint i; i < xs.length; i++) s += int256(uint256(xs[i])); return s; }
    function f7(int64 x, int128 y) external pure returns (int256) { return int256(x) + int256(y); }
}
contract Cer_cc24300_6 {
    Cee_cc24300_6 c;
    constructor() { c = new Cee_cc24300_6(); }
    function gf1(int256 a) external view returns (int256) { return int256(c.f1(a)); }
    function gf2(int256 a) external view returns (int256) { return int256(c.f2(a)); }
    function gf3(int256 a0, int256 a1) external view returns (int256) { (bool x0, bytes4 x1) = c.f3(a0, a1); return (x0 ? int256(1) : int256(0)) + int256(uint256(uint32(x1))); }
    function gf4(int256 a0, int256 a1) external view returns (int256) { (int64 x0, int64 x1) = c.f4(a0, a1); return int256(x0) + int256(x1); }
    function gf5(int256 a0, int256 a1) external view returns (int256) { Scc24300_6 memory p = c.f5(a0, a1); return c.f5x(p); }
    function gf6(int256 a) external view returns (int256) { return c.f6x(c.f6(a)); }
    function gf7(int256 a, int256 b) external view returns (int256) { return c.f7(int64(a), int128(b)); }
}
