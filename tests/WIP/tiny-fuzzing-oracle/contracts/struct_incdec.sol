// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    struct S { int8 a; uint128 b; int64 c; uint8 d; }
    S st;
    function setSt(int8 a, uint128 b, int64 c, uint8 d) external { st = S(a,b,c,d); }
    function incA() external { st.a++; }
    function decA() external { st.a--; }
    function preIncA() external returns (int8) { return ++st.a; }
    function postIncA() external returns (int8) { return st.a++; }
    function incB() external { st.b++; }
    function incC() external { st.c++; }
    function incD() external { st.d++; }   // uint8, wraps/reverts at 255
    function getA() external view returns (int8) { return st.a; }
    function getB() external view returns (uint128) { return st.b; }
    function getC() external view returns (int64) { return st.c; }
    function getD() external view returns (uint8) { return st.d; }
}
