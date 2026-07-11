// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function f(uint256 n) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](n % 3 + 1);
        for (uint256 i=0;i<x.length;i++){ x[i]=new uint256[](2); x[i][0]=i; x[i][1]=i; }
        return abi.encode(x).length;     // runtime outer, CONSTANT inner — WORKS
    }
}
