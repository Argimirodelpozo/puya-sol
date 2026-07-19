// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst47000_2, container array).
contract G_nst47000_2 {
    struct L0 { int128 a0; address a1; uint256 a2; address a3; }
    struct L1 { L0 inner; bool flag; }
    struct L2 { uint64 tag; L1 mid; }
    L2[] arr;
    function grow() external { arr.push(); }
    function set_a0(uint256 k, int128 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (int128) { if (arr.length == 0) return int128(0); return arr[k % arr.length].mid.inner.a0; }
    function bump_a0(uint256 k, int128 d) external { if (arr.length == 0) return; unchecked { arr[k % arr.length].mid.inner.a0 += d; } }
    function set_a1(uint256 k, address v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (address) { if (arr.length == 0) return address(0); return arr[k % arr.length].mid.inner.a1; }
    function set_a2(uint256 k, uint256 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a2 = v; }
    function get_a2(uint256 k) external view returns (uint256) { if (arr.length == 0) return uint256(0); return arr[k % arr.length].mid.inner.a2; }
    function bump_a2(uint256 k, uint256 d) external { if (arr.length == 0) return; unchecked { arr[k % arr.length].mid.inner.a2 += d; } }
    function set_a3(uint256 k, address v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a3 = v; }
    function get_a3(uint256 k) external view returns (address) { if (arr.length == 0) return address(0); return arr[k % arr.length].mid.inner.a3; }
    function set_tag(uint256 k, uint64 v) external { if (arr.length == 0) return; arr[k % arr.length].tag = v; }
    function get_tag(uint256 k) external view returns (uint64) { if (arr.length == 0) return uint64(0); return arr[k % arr.length].tag; }
    function del_mid(uint256 k) external { if (arr.length == 0) return; delete arr[k % arr.length].mid; }
    function roundtrip(uint256 k, uint64 nt) external { if (arr.length == 0) return; L2 memory t = arr[k % arr.length]; t.tag = nt; arr[k % arr.length] = t; }
}
