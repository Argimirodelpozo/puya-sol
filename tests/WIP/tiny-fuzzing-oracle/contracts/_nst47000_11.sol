// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst47000_11, container bare).
contract G_nst47000_11 {
    struct L0 { uint128 a0; int64 a1; int64 a2; bool a3; }
    struct L1 { L0 inner; bool flag; }
    struct L2 { uint16 tag; L1 mid; }
    L2 s;
    function set_a0(uint128 v) external { s.mid.inner.a0 = v; }
    function get_a0(uint256 _k) external view returns (uint128) { return s.mid.inner.a0; }
    function bump_a0(uint128 d) external { unchecked { s.mid.inner.a0 += d; } }
    function set_a1(int64 v) external { s.mid.inner.a1 = v; }
    function get_a1(uint256 _k) external view returns (int64) { return s.mid.inner.a1; }
    function bump_a1(int64 d) external { unchecked { s.mid.inner.a1 += d; } }
    function set_a2(int64 v) external { s.mid.inner.a2 = v; }
    function get_a2(uint256 _k) external view returns (int64) { return s.mid.inner.a2; }
    function bump_a2(int64 d) external { unchecked { s.mid.inner.a2 += d; } }
    function set_a3(bool v) external { s.mid.inner.a3 = v; }
    function get_a3(uint256 _k) external view returns (bool) { return s.mid.inner.a3; }
    function set_tag(uint16 v) external { s.tag = v; }
    function get_tag(uint256 _k) external view returns (uint16) { return s.tag; }
    function del_mid() external { delete s.mid; }
    function roundtrip(uint16 nt) external { L2 memory t = s; t.tag = nt; s = t; }
    function pickA(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a1) * 2; } }
    function pickB(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a1) - 7; } }
    function(L0 memory) internal pure returns (int64) chosen;
    function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }
    function callChosen(uint256 _k) external returns (int64) { return chosen(s.mid.inner); }
}
