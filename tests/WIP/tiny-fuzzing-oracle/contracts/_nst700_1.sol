// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst700_1, container map).
contract G_nst700_1 {
    struct L0 { int32 a0; uint128 a1; int128 a2; uint16 a3; }
    struct L1 { L0 inner; int32[] xs; bool flag; }
    struct L2 { uint128 tag; L1 mid; }
    mapping(uint256 => L2) m;
    function set_a0(uint256 k, int32 v) external { m[k].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (int32) { return m[k].mid.inner.a0; }
    function bump_a0(uint256 k, int32 d) external { unchecked { m[k].mid.inner.a0 += d; } }
    function set_a1(uint256 k, uint128 v) external { m[k].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (uint128) { return m[k].mid.inner.a1; }
    function bump_a1(uint256 k, uint128 d) external { unchecked { m[k].mid.inner.a1 += d; } }
    function set_a2(uint256 k, int128 v) external { m[k].mid.inner.a2 = v; }
    function get_a2(uint256 k) external view returns (int128) { return m[k].mid.inner.a2; }
    function bump_a2(uint256 k, int128 d) external { unchecked { m[k].mid.inner.a2 += d; } }
    function set_a3(uint256 k, uint16 v) external { m[k].mid.inner.a3 = v; }
    function get_a3(uint256 k) external view returns (uint16) { return m[k].mid.inner.a3; }
    function bump_a3(uint256 k, uint16 d) external { unchecked { m[k].mid.inner.a3 += d; } }
    function set_tag(uint256 k, uint128 v) external { m[k].tag = v; }
    function get_tag(uint256 k) external view returns (uint128) { return m[k].tag; }
    function push_xs(uint256 k, int32 v) external { m[k].mid.xs.push(v); }
    function get_xs(uint256 k, uint256 i) external view returns (int32) { if (m[k].mid.xs.length <= i) return int32(0); return m[k].mid.xs[i]; }
    function del_mid(uint256 k) external { delete m[k].mid; }
    function roundtrip(uint256 k, uint128 nt) external { L2 memory t = m[k]; t.tag = nt; m[k] = t; }
    function pickA(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a0) * 2; } }
    function pickB(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a0) - 7; } }
    function(L0 memory) internal pure returns (int64) chosen;
    function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }
    function callChosen(uint256 k) external returns (int64) { return chosen(m[k].mid.inner); }
}
