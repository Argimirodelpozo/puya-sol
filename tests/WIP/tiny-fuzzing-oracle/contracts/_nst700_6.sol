// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst700_6, container mapmap).
contract G_nst700_6 {
    struct L0 { int8 a0; int128 a1; uint64 a2; int64 a3; }
    struct L1 { L0 inner; int128[] xs; bool flag; }
    struct L2 { int128 tag; L1 mid; }
    mapping(uint256 => mapping(uint8 => L2)) mm;
    function set_a0(uint256 k, int8 v) external { mm[k][uint8(k >> 8)].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (int8) { return mm[k][uint8(k >> 8)].mid.inner.a0; }
    function bump_a0(uint256 k, int8 d) external { unchecked { mm[k][uint8(k >> 8)].mid.inner.a0 += d; } }
    function set_a1(uint256 k, int128 v) external { mm[k][uint8(k >> 8)].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (int128) { return mm[k][uint8(k >> 8)].mid.inner.a1; }
    function bump_a1(uint256 k, int128 d) external { unchecked { mm[k][uint8(k >> 8)].mid.inner.a1 += d; } }
    function set_a2(uint256 k, uint64 v) external { mm[k][uint8(k >> 8)].mid.inner.a2 = v; }
    function get_a2(uint256 k) external view returns (uint64) { return mm[k][uint8(k >> 8)].mid.inner.a2; }
    function bump_a2(uint256 k, uint64 d) external { unchecked { mm[k][uint8(k >> 8)].mid.inner.a2 += d; } }
    function set_a3(uint256 k, int64 v) external { mm[k][uint8(k >> 8)].mid.inner.a3 = v; }
    function get_a3(uint256 k) external view returns (int64) { return mm[k][uint8(k >> 8)].mid.inner.a3; }
    function bump_a3(uint256 k, int64 d) external { unchecked { mm[k][uint8(k >> 8)].mid.inner.a3 += d; } }
    function set_tag(uint256 k, int128 v) external { mm[k][uint8(k >> 8)].tag = v; }
    function get_tag(uint256 k) external view returns (int128) { return mm[k][uint8(k >> 8)].tag; }
    function push_xs(uint256 k, int128 v) external { mm[k][uint8(k >> 8)].mid.xs.push(v); }
    function get_xs(uint256 k, uint256 i) external view returns (int128) { if (mm[k][uint8(k >> 8)].mid.xs.length <= i) return int128(0); return mm[k][uint8(k >> 8)].mid.xs[i]; }
    function del_mid(uint256 k) external { delete mm[k][uint8(k >> 8)].mid; }
    function roundtrip(uint256 k, int128 nt) external { L2 memory t = mm[k][uint8(k >> 8)]; t.tag = nt; mm[k][uint8(k >> 8)] = t; }
    function pickA(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a0) * 2; } }
    function pickB(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a0) - 7; } }
    function(L0 memory) internal pure returns (int64) chosen;
    function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }
    function callChosen(uint256 k) external returns (int64) { return chosen(mm[k][uint8(k >> 8)].mid.inner); }
}
