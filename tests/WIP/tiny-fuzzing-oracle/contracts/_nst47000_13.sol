// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst47000_13, container bare).
contract G_nst47000_13 {
    struct L0 { bool a0; uint256 a1; }
    struct L1 { L0 inner; uint256[] xs; bool flag; }
    struct L2 { int128 tag; L1 mid; }
    L2 s;
    function set_a0(bool v) external { s.mid.inner.a0 = v; }
    function get_a0(uint256 _k) external view returns (bool) { return s.mid.inner.a0; }
    function set_a1(uint256 v) external { s.mid.inner.a1 = v; }
    function get_a1(uint256 _k) external view returns (uint256) { return s.mid.inner.a1; }
    function bump_a1(uint256 d) external { unchecked { s.mid.inner.a1 += d; } }
    function set_tag(int128 v) external { s.tag = v; }
    function get_tag(uint256 _k) external view returns (int128) { return s.tag; }
    function push_xs(uint256 v) external { s.mid.xs.push(v); }
    function get_xs(uint256 i) external view returns (uint256) { if (s.mid.xs.length <= i) return uint256(0); return s.mid.xs[i]; }
    function del_mid() external { delete s.mid; }
    function roundtrip(int128 nt) external { L2 memory t = s; t.tag = nt; s = t; }
}
