// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract MemSeq {
    // tuple form (single statement)
    function tup(uint256 a, uint256 b) external pure returns (uint256, uint256) {
        uint256[] memory m = new uint256[](2); m[0]=a; m[1]=b;
        (m[0], m[1]) = (m[1], m[0]);
        return (m[0], m[1]);
    }
    // sequential form via temps (should chain)
    function seq(uint256 a, uint256 b) external pure returns (uint256, uint256) {
        uint256[] memory m = new uint256[](2); m[0]=a; m[1]=b;
        uint256 t0 = m[1]; uint256 t1 = m[0];
        m[0] = t0;
        m[1] = t1;
        return (m[0], m[1]);
    }
    // two independent writes, no aliasing between them
    function twowrite(uint256 a, uint256 b) external pure returns (uint256, uint256) {
        uint256[] memory m = new uint256[](2); m[0]=0; m[1]=0;
        m[0] = a;
        m[1] = b;
        return (m[0], m[1]);
    }
    // tuple with constants (no self-read aliasing)
    function tupconst(uint256 a, uint256 b) external pure returns (uint256, uint256) {
        uint256[] memory m = new uint256[](2); m[0]=0; m[1]=0;
        (m[0], m[1]) = (a, b);
        return (m[0], m[1]);
    }
}
