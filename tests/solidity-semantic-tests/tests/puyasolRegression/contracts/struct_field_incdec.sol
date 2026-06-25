// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Found by the differential fuzzer: struct STATE-VAR field ++/-- (st.x++) failed to compile
// ('unsupported assignment target') whenever the contract has 2+ functions (struct stays boxed); the
// inc/dec path emitted a bare field write instead of the struct copy-on-write the compound path uses.
contract C {
    struct S { int8 a; uint128 b; int64 c; uint8 d; } S st;
    struct Inner { uint64 x; int32 y; } struct Outer { Inner inner; uint128 z; } Outer o;
    function setSt(int8 a, uint128 b, int64 c, uint8 d) external { st = S(a,b,c,d); }
    function postIncA() external returns (int8) { return st.a++; }
    function preIncA() external returns (int8) { return ++st.a; }
    function decC() external { st.c--; }
    function incD() external { st.d++; }
    function getA() external view returns (int8){return st.a;}
    function getB() external view returns (uint128){return st.b;}
    function getC() external view returns (int64){return st.c;}
    function getD() external view returns (uint8){return st.d;}
    function setO(uint64 x, int32 y, uint128 z) external { o = Outer(Inner(x,y), z); }
    function incNX() external returns (uint64) { return o.inner.x++; }
    function getNX() external view returns (uint64){return o.inner.x;}
    function getNY() external view returns (int32){return o.inner.y;}
    function f2() external pure returns (uint256){return 1;}  // forces boxing
}
