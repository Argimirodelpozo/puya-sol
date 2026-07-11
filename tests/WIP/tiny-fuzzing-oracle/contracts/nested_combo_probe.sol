// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Hand probe for the nested-aggregate type-system push: nested structs x mappings x
// function pointers, each combo exercised once. Differential via fuzz_state.py.
contract G {
    struct Leaf { int64 v; uint128 w; bool b; }
    struct Mid  { uint64 tag; Leaf leaf; int32 x; }
    struct FpBox { uint64 tag; function(int64) internal pure returns (int64) f; }

    Mid s;                                            // bare nested storage struct
    mapping(uint256 => Mid) m;                        // map -> nested struct
    mapping(uint256 => mapping(uint8 => Leaf)) mm;    // nested map -> struct
    Leaf[] list;                                      // dyn array of structs
    function(int64) internal pure returns (int64) fp; // storage funcptr
    FpBox fbox;                                       // funcptr INSIDE a struct
    mapping(uint8 => function(int64) internal pure returns (int64)) fpm; // map -> funcptr
    function(int64) internal pure returns (int64)[3] fparr;              // funcptr fixed array

    function dbl(int64 a) internal pure returns (int64) { return a * 2; }
    function neg(int64 a) internal pure returns (int64) { return -a; }
    function inc(int64 a) internal pure returns (int64) { unchecked { return a + 1; } }

    // nested struct deep leaf writes/reads through every container
    function setS(uint64 t, int64 v, uint128 w, bool b, int32 x) external { s = Mid(t, Leaf(v, w, b), x); }
    function setSLeafV(int64 v) external { s.leaf.v = v; }
    function bumpSLeafV(int64 d) external { unchecked { s.leaf.v += d; } }
    function getSLeafV() external view returns (int64) { return s.leaf.v; }
    function getSLeafW() external view returns (uint128) { return s.leaf.w; }
    function setM(uint256 k, int64 v) external { m[k].leaf.v = v; m[k].tag = uint64(k); }
    function bumpM(uint256 k, int64 d) external { unchecked { m[k].leaf.v += d; } }
    function getM(uint256 k) external view returns (int64) { return m[k].leaf.v; }
    function getMTag(uint256 k) external view returns (uint64) { return m[k].tag; }
    function setMM(uint256 k1, uint8 k2, int64 v, bool b) external { mm[k1][k2] = Leaf(v, uint128(uint64(k2)) + 7, b); }
    function getMM(uint256 k1, uint8 k2) external view returns (int64) { return mm[k1][k2].v; }
    function getMMB(uint256 k1, uint8 k2) external view returns (bool) { return mm[k1][k2].b; }
    function pushList(int64 v) external { list.push(Leaf(v, 1, true)); }
    function setListLeaf(uint256 i, int64 v) external { if (i < list.length) list[i].v = v; }
    function getList(uint256 i) external view returns (int64) { return i < list.length ? list[i].v : int64(0); }
    function delSLeaf() external { delete s.leaf; }

    // funcptr combos
    function pick(uint8 which) external { fp = which == 0 ? dbl : which == 1 ? neg : inc; }
    function callFp(int64 a) external view returns (int64) { return fp(a); }
    function setBox(uint8 which, uint64 t) external { fbox.tag = t; fbox.f = which == 0 ? dbl : neg; }
    function callBox(int64 a) external view returns (int64) { return fbox.f(a); }
    function getBoxTag() external view returns (uint64) { return fbox.tag; }
    function setFpm(uint8 k, uint8 which) external { fpm[k] = which == 0 ? dbl : which == 1 ? neg : inc; }
    function callFpm(uint8 k, int64 a) external view returns (int64) { return fpm[k](a); }
    function fillArr() external { fparr[0] = dbl; fparr[1] = neg; fparr[2] = inc; }
    function callArr(uint8 i, int64 a) external view returns (int64) { return fparr[i % 3](a); }

    // funcptr taking/returning the nested struct (memory)
    function sumLeaf(Leaf memory l) internal pure returns (int64) { unchecked { return l.v + int64(uint64(l.w)) + (l.b ? int64(1) : int64(0)); } }
    function zeroLeaf(Leaf memory l) internal pure returns (int64) { return l.b ? int64(0) : l.v; }
    function(Leaf memory) internal pure returns (int64) sfp;
    function pickS(uint8 which) external { sfp = which == 0 ? sumLeaf : zeroLeaf; }
    function callSfpOnS(int64 v, uint128 w, bool b) external view returns (int64) { return sfp(Leaf(v, w, b)); }
    function callSfpOnStored(uint256 k) external view returns (int64) { return sfp(m[k].leaf); }
}
