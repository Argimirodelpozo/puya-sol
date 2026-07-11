// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function localAdd(int128 a, int128 d) external pure returns (int128) { int128 x = a; x += d; return x; }
    function localSub(int128 a, int128 d) external pure returns (int128) { int128 x = a; x -= d; return x; }
    function localMul(int128 a, int128 d) external pure returns (int128) { int128 x = a; x *= d; return x; }
    function plainAdd(int128 a, int128 d) external pure returns (int128) { return a + d; }  // control
}
