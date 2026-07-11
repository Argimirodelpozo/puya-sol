// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { int128 x; }
    S public s;                                                  // auto-getter s()
    function addX(int128 d)  external { s.x += d; }              // compound store
    function rd()  external view returns (int128) { return s.x; } // explicit read of SAME value
    function setEq(int128 d) external { s.x = s.x + d; }         // computed store, NOT += syntax
}
