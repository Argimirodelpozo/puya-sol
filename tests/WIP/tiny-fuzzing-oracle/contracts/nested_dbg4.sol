// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract NestedDbg4 {
    function encOnly(uint256 n, uint256 m) external pure returns (uint256) {
        uint256[][] memory x = new uint256[][](n % 3 + 1);
        for (uint256 i=0;i<x.length;i++){ x[i]=new uint256[](m % 3); for(uint256 j=0;j<x[i].length;j++) x[i][j]=i*10+j; }
        return abi.encode(x).length;     // encode only
    }
}
