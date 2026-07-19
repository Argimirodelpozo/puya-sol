// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst24400_0, container bare).
contract G_nst24400_0 {
    struct L0 { address a0; int8 a1; int128 a2; }
    struct L1 { L0 inner; uint16[] xs; bool flag; }
    struct L2 { uint128 tag; L1 mid; }
    L2 s;
    function set_a0(address v) external { s.mid.inner.a0 = v; }
    function get_a0(uint256 _k) external view returns (address) { return s.mid.inner.a0; }
    function set_a1(int8 v) external { s.mid.inner.a1 = v; }
    function get_a1(uint256 _k) external view returns (int8) { return s.mid.inner.a1; }
    function bump_a1(int8 d) external { unchecked { s.mid.inner.a1 += d; } }
    function set_a2(int128 v) external { s.mid.inner.a2 = v; }
    function get_a2(uint256 _k) external view returns (int128) { return s.mid.inner.a2; }
    function bump_a2(int128 d) external { unchecked { s.mid.inner.a2 += d; } }
    function set_tag(uint128 v) external { s.tag = v; }
    function get_tag(uint256 _k) external view returns (uint128) { return s.tag; }
    function push_xs(uint16 v) external { s.mid.xs.push(v); }
    function get_xs(uint256 i) external view returns (uint16) { if (s.mid.xs.length <= i) return uint16(0); return s.mid.xs[i]; }
    function del_mid() external { delete s.mid; }
    function roundtrip(uint128 nt) external { L2 memory t = s; t.tag = nt; s = t; }
    function pickA(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a1) * 2; } }
    function pickB(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a1) - 7; } }
    function(L0 memory) internal pure returns (int64) chosen;
    function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }
    function callChosen(uint256 _k) external returns (int64) { return chosen(s.mid.inner); }
}
