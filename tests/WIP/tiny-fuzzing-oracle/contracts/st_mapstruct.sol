// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// mapping(K => Struct) with compound ops on struct fields, across calls.
contract MS {
    struct P { uint128 a; int128 b; uint64 c; }
    mapping(uint256 => P) public ps;
    function setA(uint256 k, uint128 v) external { ps[k].a = v; }
    function setB(uint256 k, int128 v)  external { ps[k].b = v; }
    function incA(uint256 k, uint128 d) external { ps[k].a += d; }   // unsigned compound on mapping field
    function addB(uint256 k, int128 d)  external { ps[k].b += d; }   // SIGNED compound on mapping field
    function setC(uint256 k, uint64 v)  external { ps[k].c = v; }
    function del(uint256 k)             external { delete ps[k]; }
}
