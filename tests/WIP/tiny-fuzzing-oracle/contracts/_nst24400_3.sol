// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst24400_3, container array).
contract G_nst24400_3 {
    struct L0 { int32 a0; bytes4 a1; uint16 a2; }
    struct L1 { L0 inner; bool flag; }
    struct L2 { uint256 tag; L1 mid; }
    L2[] arr;
    function grow() external { arr.push(); }
    function set_a0(uint256 k, int32 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (int32) { if (arr.length == 0) return int32(0); return arr[k % arr.length].mid.inner.a0; }
    function bump_a0(uint256 k, int32 d) external { if (arr.length == 0) return; unchecked { arr[k % arr.length].mid.inner.a0 += d; } }
    function set_a1(uint256 k, bytes4 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (bytes4) { if (arr.length == 0) return bytes4(0); return arr[k % arr.length].mid.inner.a1; }
    function set_a2(uint256 k, uint16 v) external { if (arr.length == 0) return; arr[k % arr.length].mid.inner.a2 = v; }
    function get_a2(uint256 k) external view returns (uint16) { if (arr.length == 0) return uint16(0); return arr[k % arr.length].mid.inner.a2; }
    function bump_a2(uint256 k, uint16 d) external { if (arr.length == 0) return; unchecked { arr[k % arr.length].mid.inner.a2 += d; } }
    function set_tag(uint256 k, uint256 v) external { if (arr.length == 0) return; arr[k % arr.length].tag = v; }
    function get_tag(uint256 k) external view returns (uint256) { if (arr.length == 0) return uint256(0); return arr[k % arr.length].tag; }
    function del_mid(uint256 k) external { if (arr.length == 0) return; delete arr[k % arr.length].mid; }
    function roundtrip(uint256 k, uint256 nt) external { if (arr.length == 0) return; L2 memory t = arr[k % arr.length]; t.tag = nt; arr[k % arr.length] = t; }
    function pickA(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a0) * 2; } }
    function pickB(L0 memory p) internal pure returns (int64) { unchecked { return int64(p.a0) - 7; } }
    function(L0 memory) internal pure returns (int64) chosen;
    function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }
    function callChosen(uint256 k) external returns (int64) { if (arr.length == 0) return -1; return chosen(arr[k % arr.length].mid.inner); }
}
