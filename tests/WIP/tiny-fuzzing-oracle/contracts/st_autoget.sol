// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct S { int128 x; uint64 y; }
    S public s;                          // auto-getter s() -> (int128, uint64)
    mapping(uint256 => S) public ms;     // auto-getter ms(k)
    int128 public plain;                 // plain auto-getter (control — known good)
    function setX(int128 v)             external { s.x = v; }
    function setMsX(uint256 k, int128 v) external { ms[k].x = v; }
    function setPlain(int128 v)         external { plain = v; }
    function rtX()                      external view returns (int128) { return s.x; }   // explicit getter (control)
}
