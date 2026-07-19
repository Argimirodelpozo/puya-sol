// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst47000_7, container map).
contract G_nst47000_7 {
    struct L0 { uint128 a0; bytes4 a1; bytes4 a2; }
    struct L1 { L0 inner; bool flag; }
    struct L2 { int8 tag; L1 mid; }
    mapping(uint256 => L2) m;
    function set_a0(uint256 k, uint128 v) external { m[k].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (uint128) { return m[k].mid.inner.a0; }
    function bump_a0(uint256 k, uint128 d) external { unchecked { m[k].mid.inner.a0 += d; } }
    function set_a1(uint256 k, bytes4 v) external { m[k].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (bytes4) { return m[k].mid.inner.a1; }
    function set_a2(uint256 k, bytes4 v) external { m[k].mid.inner.a2 = v; }
    function get_a2(uint256 k) external view returns (bytes4) { return m[k].mid.inner.a2; }
    function set_tag(uint256 k, int8 v) external { m[k].tag = v; }
    function get_tag(uint256 k) external view returns (int8) { return m[k].tag; }
    function del_mid(uint256 k) external { delete m[k].mid; }
    function roundtrip(uint256 k, int8 nt) external { L2 memory t = m[k]; t.tag = nt; m[k] = t; }
}
