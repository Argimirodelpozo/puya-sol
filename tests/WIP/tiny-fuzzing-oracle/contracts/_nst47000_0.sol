// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst47000_0, container array).
contract G_nst47000_0 {
    struct L0 { uint128 a0; uint64 a1; }
    struct L1 { L0 inner; bool flag; }
    struct L2 { uint64 tag; L1 mid; }
    L2[] arr;
    function grow() external { arr.push(); }
    function set_a0(uint256 k, uint128 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (uint128) { if (arr.length == 0) return uint128(0); return arr[k % arr.length].mid.inner.a0; }
    function bump_a0(uint256 k, uint128 d) external { if (arr.length == 0) return; unchecked { arr[k % arr.length].mid.inner.a0 += d; } }
    function set_a1(uint256 k, uint64 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (uint64) { if (arr.length == 0) return uint64(0); return arr[k % arr.length].mid.inner.a1; }
    function bump_a1(uint256 k, uint64 d) external { if (arr.length == 0) return; unchecked { arr[k % arr.length].mid.inner.a1 += d; } }
    function set_tag(uint256 k, uint64 v) external { if (arr.length == 0) return; arr[k % arr.length].tag = v; }
    function get_tag(uint256 k) external view returns (uint64) { if (arr.length == 0) return uint64(0); return arr[k % arr.length].tag; }
    function del_mid(uint256 k) external { if (arr.length == 0) return; delete arr[k % arr.length].mid; }
    function roundtrip(uint256 k, uint64 nt) external { if (arr.length == 0) return; L2 memory t = arr[k % arr.length]; t.tag = nt; arr[k % arr.length] = t; }
    function pickA(L0 memory p) internal pure returns (int64) { unchecked { return int64(uint64(p.a1)) * 2; } }
    function pickB(L0 memory p) internal pure returns (int64) { unchecked { return int64(uint64(p.a1)) - 7; } }
    function(L0 memory) internal pure returns (int64) chosen;
    function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }
    function callChosen(uint256 k) external returns (int64) { if (arr.length == 0) return -1; return chosen(arr[k % arr.length].mid.inner); }
}
