// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    struct Inner { uint64 x; int32 y; }
    struct Outer { Inner inner; uint128 z; }
    Outer o;
    uint8[] sink;  // 2nd state ref / extra fn forces boxing
    function setO(uint64 x, int32 y, uint128 z) external { o = Outer(Inner(x,y), z); }
    function incNestedX() external returns (uint64) { return o.inner.x++; }
    function incNestedY() external { o.inner.y++; }
    function incZ() external { o.z++; }
    function getX() external view returns (uint64){return o.inner.x;}
    function getY() external view returns (int32){return o.inner.y;}
    function getZ() external view returns (uint128){return o.z;}
    function f2() external pure returns (uint256){return 7;}
}
