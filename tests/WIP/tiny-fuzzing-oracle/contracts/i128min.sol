// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    int128 public sv;
    struct S { int128 x; }
    S public st;
    function setSv(int128 v)      external returns (int128) { sv = v; return sv; }        // state var set+read
    function setStX(int128 v)     external returns (int128) { st.x = v; return st.x; }     // struct field set+read
    function compoundStX(int128 v) external returns (int128) { st.x = 0; st.x += v; return st.x; }
    function autoSv()             external view returns (int128) { return sv; }            // (sv() auto-getter equiv)
}
