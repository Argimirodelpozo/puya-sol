// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst47000_1, container array).
contract G_nst47000_1 {
    struct L0 { uint128 a0; uint256 a1; }
    struct L1 { L0 inner; uint16[] xs; bool flag; }
    struct L2 { int64 tag; L1 mid; }
    L2[] arr;
    function grow() external { arr.push(); }
    function set_a0(uint256 k, uint128 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (uint128) { if (arr.length == 0) return uint128(0); return arr[k % arr.length].mid.inner.a0; }
    function bump_a0(uint256 k, uint128 d) external { if (arr.length == 0) return; unchecked { arr[k % arr.length].mid.inner.a0 += d; } }
    function set_a1(uint256 k, uint256 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (uint256) { if (arr.length == 0) return uint256(0); return arr[k % arr.length].mid.inner.a1; }
    function bump_a1(uint256 k, uint256 d) external { if (arr.length == 0) return; unchecked { arr[k % arr.length].mid.inner.a1 += d; } }
    function set_tag(uint256 k, int64 v) external { if (arr.length == 0) return; arr[k % arr.length].tag = v; }
    function get_tag(uint256 k) external view returns (int64) { if (arr.length == 0) return int64(0); return arr[k % arr.length].tag; }
    function push_xs(uint256 k, uint16 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.xs.push(v); }
    function get_xs(uint256 k, uint256 i) external view returns (uint16) { if (arr.length == 0) return uint16(0); if (arr[k % arr.length].mid.xs.length <= i) return uint16(0); return arr[k % arr.length].mid.xs[i]; }
    function del_mid(uint256 k) external { if (arr.length == 0) return; delete arr[k % arr.length].mid; }
    function roundtrip(uint256 k, int64 nt) external { if (arr.length == 0) return; L2 memory t = arr[k % arr.length]; t.tag = nt; arr[k % arr.length] = t; }
}
