// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { int16 x; }     // single SIGNED <=64-bit field
    S public s;
    function setX(int16 d) external { s.x = d; }
}
