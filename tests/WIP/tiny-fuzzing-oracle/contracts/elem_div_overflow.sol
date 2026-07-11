// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    int128[] arr;
    struct S { int128 a; int256 b; }
    S st;
    int128[3] fixedArr;
    function pushA(int128 v) external { arr.push(v); }
    function divArr(uint256 i, int128 b) external returns (int128) { arr[i] /= b; return arr[i]; }
    function setSt(int128 a, int256 b) external { st = S(a, b); }
    function divStA(int128 b) external returns (int128) { st.a /= b; return st.a; }
    function divStB(int256 b) external returns (int256) { st.b /= b; return st.b; }
    function setFixed(uint256 i, int128 v) external { fixedArr[i] = v; }
    function divFixed(uint256 i, int128 b) external returns (int128) { fixedArr[i] /= b; return fixedArr[i]; }
    // memory local array element
    function divMemElem(int128 a, int128 b) external pure returns (int128) {
        int128[2] memory m; m[0] = a; m[0] /= b; return m[0];
    }
}
