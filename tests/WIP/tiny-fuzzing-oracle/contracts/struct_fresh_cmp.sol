// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { int8 a; uint128 b; int64 c; uint8 d; } S st;
    function addB() external { st.b += 1; }     // compound on fresh
    function incB() external { st.b++; }         // inc on fresh
    function getB() external view returns (uint128) { return st.b; }
    function f2() external pure returns (uint256) { return 1; }  // 2nd fn forces boxing
}
