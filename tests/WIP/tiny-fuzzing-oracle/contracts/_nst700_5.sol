// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst700_5, container map).
contract G_nst700_5 {
    struct L0 { bytes4 a0; bytes4 a1; }
    struct L1 { L0 inner; uint256[] xs; bool flag; }
    struct L2 { uint64 tag; L1 mid; }
    mapping(uint256 => L2) m;
    function set_a0(uint256 k, bytes4 v) external { m[k].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (bytes4) { return m[k].mid.inner.a0; }
    function set_a1(uint256 k, bytes4 v) external { m[k].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (bytes4) { return m[k].mid.inner.a1; }
    function set_tag(uint256 k, uint64 v) external { m[k].tag = v; }
    function get_tag(uint256 k) external view returns (uint64) { return m[k].tag; }
    function push_xs(uint256 k, uint256 v) external { m[k].mid.xs.push(v); }
    function get_xs(uint256 k, uint256 i) external view returns (uint256) { if (m[k].mid.xs.length <= i) return uint256(0); return m[k].mid.xs[i]; }
    function del_mid(uint256 k) external { delete m[k].mid; }
    function roundtrip(uint256 k, uint64 nt) external { L2 memory t = m[k]; t.tag = nt; m[k] = t; }
}
