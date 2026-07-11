// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    int128 s;
    int256 s2;
    mapping(uint256 => int128) m;
    function setS(int128 v) external { s = v; }
    function divS(int128 b) external returns (int128) { s /= b; return s; }
    function setS2(int256 v) external { s2 = v; }
    function divS2(int256 b) external returns (int256) { s2 /= b; return s2; }
    function setM(uint256 k, int128 v) external { m[k] = v; }
    function divM(uint256 k, int128 b) external returns (int128) { m[k] /= b; return m[k]; }
}
