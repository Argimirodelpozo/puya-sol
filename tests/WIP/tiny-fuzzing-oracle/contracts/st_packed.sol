// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Packed sub-word neighbours mutated across calls — does writing one corrupt its slot-mates?
contract PN {
    uint64 public a; uint64 public b; uint64 public c; uint64 public d;
    int128 public x; int128 public y;
    function setA(uint64 v)  external { a = v; }
    function incA(uint64 v)  external { a += v; }
    function setD(uint64 v)  external { d = v; }
    function setX(int128 v)  external { x = v; }
    function addX(int128 v)  external { x += v; }
    function subY(int128 v)  external { y -= v; }
    function setAll(uint64 va, uint64 vb, uint64 vc) external { a = va; b = vb; c = vc; }
}
