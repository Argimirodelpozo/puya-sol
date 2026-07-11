// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Computational probe over arrays / structs / tuples — clean diff signal (no by-design noise).
contract Probe {
    function sum(uint256[] calldata a) external pure returns (uint256 s) {
        for (uint i = 0; i < a.length; i++) s += a[i];            // checked add → overflow reverts
    }
    function maxOf(int128[] calldata a) external pure returns (int128 m) {
        m = type(int128).min;
        for (uint i = 0; i < a.length; i++) if (a[i] > m) m = a[i];  // signed compare
    }
    function rev3(uint64[3] calldata a) external pure returns (uint64[3] memory r) {
        r[0] = a[2]; r[1] = a[1]; r[2] = a[0];                    // fixed-array return
    }
    struct P { int128 x; uint64 y; }
    function addP(P calldata p, int128 d) external pure returns (P memory) {
        return P(p.x + d, p.y + 1);                              // struct param + struct return
    }
    function pair(uint256 a, uint256 b) external pure returns (uint256, uint256) {
        return (a + b, a * b);                                   // multi-return tuple
    }
    function nested(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i = 0; i < a.length; i++)
            for (uint j = 0; j < a[i].length; j++) s += a[i][j]; // nested dynamic array
    }
}
