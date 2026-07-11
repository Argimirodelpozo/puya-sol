// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract SweptOOB {
    uint256[200] big;   // > 4096 bytes => multi-box paged
    function mbRead(uint256 i)  external returns (uint256) { big[0]=1; big[1]=2; return big[i]; }   // i>=200 / huge reverts
    function mbWrite(uint256 i) external returns (uint256) { big[i]=5; return big[0]; }
    function inB(uint256 i)     external returns (uint256) { big[5]=99; return big[i]; }            // i=5 => 99
}
