// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    struct S { uint64 a; uint64 b; } S st;
    function setSt(uint64 a, uint64 b) external { st = S(a,b); }
    function incA() external { st.a++; }
    function decA() external { st.a--; }
    function preIncA() external returns (uint64) { return ++st.a; }
    function postIncA() external returns (uint64) { return st.a++; }
    function getA() external view returns (uint64) { return st.a; }
    function getB() external view returns (uint64) { return st.b; }
}
