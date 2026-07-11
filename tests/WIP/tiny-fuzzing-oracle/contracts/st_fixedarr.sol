// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract FA {
    uint256[5] public arr;
    int128[4] public sarr;
    function set(uint256 i, uint256 v) external { arr[i] = v; }    // OOB reverts (i >= 5)
    function inc(uint256 i, uint256 v) external { arr[i] += v; }
    function addS(uint256 i, int128 d) external { sarr[i] += d; }  // signed compound on fixed array element
    function sum()                     external view returns (uint256) { uint256 s; for(uint256 i=0;i<5;i++) s+=arr[i]; return s; }
}
