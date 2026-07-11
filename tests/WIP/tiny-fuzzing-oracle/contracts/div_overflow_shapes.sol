// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    int128[3] fa;
    struct S { int128 a; }
    S st;
    int128[][] nest;
    function setFa(uint256 i, int128 v) external { fa[i] = v; }
    function divFa(uint256 i, int128 b) external returns (int128) { fa[i] /= b; return fa[i]; }
    function setSt(int128 v) external { st.a = v; }
    function divSt(int128 b) external returns (int128) { st.a /= b; return st.a; }
    function initNest(int128 v) external { nest.push(); nest[0].push(v); }
    function divNest(int128 b) external returns (int128) { nest[0][0] /= b; return nest[0][0]; }
    function divMemDyn(int128 a, int128 b) external pure returns (int128) {
        int128[] memory m = new int128[](2); m[0] = a; m[0] /= b; return m[0];
    }
}
