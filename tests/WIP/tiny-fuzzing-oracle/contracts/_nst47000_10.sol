// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst47000_10, container map).
contract G_nst47000_10 {
    struct L0 { int128 a0; address a1; uint64 a2; uint16 a3; }
    struct L1 { L0 inner; bool flag; }
    struct L2 { int64 tag; L1 mid; }
    mapping(uint256 => L2) m;
    function set_a0(uint256 k, int128 v) external { m[k].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (int128) { return m[k].mid.inner.a0; }
    function bump_a0(uint256 k, int128 d) external { unchecked { m[k].mid.inner.a0 += d; } }
    function set_a1(uint256 k, address v) external { m[k].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (address) { return m[k].mid.inner.a1; }
    function set_a2(uint256 k, uint64 v) external { m[k].mid.inner.a2 = v; }
    function get_a2(uint256 k) external view returns (uint64) { return m[k].mid.inner.a2; }
    function bump_a2(uint256 k, uint64 d) external { unchecked { m[k].mid.inner.a2 += d; } }
    function set_a3(uint256 k, uint16 v) external { m[k].mid.inner.a3 = v; }
    function get_a3(uint256 k) external view returns (uint16) { return m[k].mid.inner.a3; }
    function bump_a3(uint256 k, uint16 d) external { unchecked { m[k].mid.inner.a3 += d; } }
    function set_tag(uint256 k, int64 v) external { m[k].tag = v; }
    function get_tag(uint256 k) external view returns (int64) { return m[k].tag; }
    function del_mid(uint256 k) external { delete m[k].mid; }
    function roundtrip(uint256 k, int64 nt) external { L2 memory t = m[k]; t.tag = nt; m[k] = t; }
    function pickA(L0 memory p) internal pure returns (int64) { unchecked { return int64(uint64(p.a2)) * 2; } }
    function pickB(L0 memory p) internal pure returns (int64) { unchecked { return int64(uint64(p.a2)) - 7; } }
    function(L0 memory) internal pure returns (int64) chosen;
    function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }
    function callChosen(uint256 k) external returns (int64) { return chosen(m[k].mid.inner); }
}
