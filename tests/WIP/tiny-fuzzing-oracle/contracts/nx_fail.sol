// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function f(uint256 n, uint256 m) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](n % 3 + 1);
        for (uint256 i=0;i<x.length;i++){ uint256 L = m % 3; x[i]=new uint256[](L); for(uint256 j=0;j<L;j++) x[i][j]=i; }
        return abi.encode(x).length;     // runtime outer, RUNTIME inner — FAILS
    }
}
