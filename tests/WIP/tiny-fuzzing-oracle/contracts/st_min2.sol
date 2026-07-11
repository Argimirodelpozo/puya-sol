// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { uint64 a; int128 x; }
    S public s;                              // x at offset 8 (after one full-width field)
    function addX(int128 d) external { s.x += d; }    // compound, 2nd field
}
