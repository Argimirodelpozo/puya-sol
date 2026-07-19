// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst47000_9, container bare).
contract G_nst47000_9 {
    struct L0 { uint16 a0; uint128 a1; int8 a2; }
    struct L1 { L0 inner; int8[] xs; bool flag; }
    struct L2 { int8 tag; L1 mid; }
    L2 s;
    function set_a0(uint16 v) external { s.mid.inner.a0 = v; }
    function get_a0(uint256 _k) external view returns (uint16) { return s.mid.inner.a0; }
    function bump_a0(uint16 d) external { unchecked { s.mid.inner.a0 += d; } }
    function set_a1(uint128 v) external { s.mid.inner.a1 = v; }
    function get_a1(uint256 _k) external view returns (uint128) { return s.mid.inner.a1; }
    function bump_a1(uint128 d) external { unchecked { s.mid.inner.a1 += d; } }
    function set_a2(int8 v) external { s.mid.inner.a2 = v; }
    function get_a2(uint256 _k) external view returns (int8) { return s.mid.inner.a2; }
    function bump_a2(int8 d) external { unchecked { s.mid.inner.a2 += d; } }
    function set_tag(int8 v) external { s.tag = v; }
    function get_tag(uint256 _k) external view returns (int8) { return s.tag; }
    function push_xs(int8 v) external { s.mid.xs.push(v); }
    function get_xs(uint256 i) external view returns (int8) { if (s.mid.xs.length <= i) return int8(0); return s.mid.xs[i]; }
    function del_mid() external { delete s.mid; }
    function roundtrip(int8 nt) external { L2 memory t = s; t.tag = nt; s = t; }
    function pickA(L0 memory p) internal pure returns (int64) { unchecked { return int64(uint64(p.a0)) * 2; } }
    function pickB(L0 memory p) internal pure returns (int64) { unchecked { return int64(uint64(p.a0)) - 7; } }
    function(L0 memory) internal pure returns (int64) chosen;
    function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }
    function callChosen(uint256 _k) external returns (int64) { return chosen(s.mid.inner); }
}
