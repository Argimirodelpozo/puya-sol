// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { int128 x; }
    S public s;                              // single-field, x at offset 0
    function addX(int128 d) external { s.x += d; }    // compound, first field
}
