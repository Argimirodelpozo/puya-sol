// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED nested-aggregate fixture (fuzz_nested.py, tag nst700_4, container mapmap).
contract G_nst700_4 {
    struct L0 { address a0; bytes32 a1; uint16 a2; }
    struct L1 { L0 inner; bool flag; }
    struct L2 { int32 tag; L1 mid; }
    mapping(uint256 => mapping(uint8 => L2)) mm;
    function set_a0(uint256 k, address v) external { mm[k][uint8(k >> 8)].mid.inner.a0 = v; }
    function get_a0(uint256 k) external view returns (address) { return mm[k][uint8(k >> 8)].mid.inner.a0; }
    function set_a1(uint256 k, bytes32 v) external { mm[k][uint8(k >> 8)].mid.inner.a1 = v; }
    function get_a1(uint256 k) external view returns (bytes32) { return mm[k][uint8(k >> 8)].mid.inner.a1; }
    function set_a2(uint256 k, uint16 v) external { mm[k][uint8(k >> 8)].mid.inner.a2 = v; }
    function get_a2(uint256 k) external view returns (uint16) { return mm[k][uint8(k >> 8)].mid.inner.a2; }
    function bump_a2(uint256 k, uint16 d) external { unchecked { mm[k][uint8(k >> 8)].mid.inner.a2 += d; } }
    function set_tag(uint256 k, int32 v) external { mm[k][uint8(k >> 8)].tag = v; }
    function get_tag(uint256 k) external view returns (int32) { return mm[k][uint8(k >> 8)].tag; }
    function del_mid(uint256 k) external { delete mm[k][uint8(k >> 8)].mid; }
    function roundtrip(uint256 k, int32 nt) external { L2 memory t = mm[k][uint8(k >> 8)]; t.tag = nt; mm[k][uint8(k >> 8)] = t; }
    function pickA(L0 memory p) internal pure returns (int64) { unchecked { return int64(uint64(p.a2)) * 2; } }
    function pickB(L0 memory p) internal pure returns (int64) { unchecked { return int64(uint64(p.a2)) - 7; } }
    function(L0 memory) internal pure returns (int64) chosen;
    function choose(uint8 w) external { chosen = w == 0 ? pickA : pickB; }
    function callChosen(uint256 k) external returns (int64) { return chosen(mm[k][uint8(k >> 8)].mid.inner); }
}
