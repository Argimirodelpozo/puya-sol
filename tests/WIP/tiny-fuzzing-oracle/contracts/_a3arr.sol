// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A {
    function mk(uint256 a) external pure returns (uint32[] memory) { uint32[] memory r = new uint32[](3); r[0]=uint32(a); r[1]=uint32(a)+1; r[2]=uint32(a)+2; return r; }
    function mkb(uint256 a) external pure returns (uint256[] memory) { uint256[] memory r = new uint256[](2); r[0]=a; r[1]=a*2; return r; }
    function sum(uint32[] memory xs) external pure returns (uint256) { uint256 s; for(uint i;i<xs.length;i++) s+=xs[i]; return s; }
}
