// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { uint64 a; uint64 b; } S st;   // st never initialized
    function incA() external { st.a++; }              // inc on fresh struct
    function addA() external { st.a += 1; }           // compound on fresh struct
    function getA() external view returns (uint64) { return st.a; }
    function getB() external view returns (uint64) { return st.b; }
}
