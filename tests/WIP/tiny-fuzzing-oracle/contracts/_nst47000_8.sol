// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst47000_8, container mapmap).
contract G_nst47000_8 {
    struct L0 { int128 a0; bool a1; }
    struct L1 { L0 inner; uint16[] xs; bool flag; }
    struct L2 { int32 tag; L1 mid; }
    mapping(uint256 => mapping(uint8 => L2)) mm;
    function set_a0(uint256 k, int128 v) external { mm[k][uint8(k >> 8)].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (int128) { return mm[k][uint8(k >> 8)].mid.inner.a0; }
    function bump_a0(uint256 k, int128 d) external { unchecked { mm[k][uint8(k >> 8)].mid.inner.a0 += d; } }
    function set_a1(uint256 k, bool v) external { mm[k][uint8(k >> 8)].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (bool) { return mm[k][uint8(k >> 8)].mid.inner.a1; }
    function set_tag(uint256 k, int32 v) external { mm[k][uint8(k >> 8)].tag = v; }
    function get_tag(uint256 k) external view returns (int32) { return mm[k][uint8(k >> 8)].tag; }
    function push_xs(uint256 k, uint16 v) external { mm[k][uint8(k >> 8)].mid.xs.push(v); }
    function get_xs(uint256 k, uint256 i) external view returns (uint16) { if (mm[k][uint8(k >> 8)].mid.xs.length <= i) return uint16(0); return mm[k][uint8(k >> 8)].mid.xs[i]; }
    function del_mid(uint256 k) external { delete mm[k][uint8(k >> 8)].mid; }
    function roundtrip(uint256 k, int32 nt) external { L2 memory t = mm[k][uint8(k >> 8)]; t.tag = nt; mm[k][uint8(k >> 8)] = t; }
}
