// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the differential fuzzer.
// An array index >= 2^64 must REVERT (out of bounds), not silently truncate to uint64 and read
// arr[low-64-bits]. Covers storage-dynamic, memory, and fixed-size arrays (read + write paths).
contract ArrayOOB {
    uint256[] sArr;
    uint256[3] fArr;
    uint256[200] mbArr;  // > 4096 bytes => multi-box paged
    function storageOOB(uint256 i) external returns (uint256) { delete sArr; sArr.push(9); sArr.push(8); return sArr[i]; }
    function memOOB(uint256 i)     external pure returns (uint256) { uint256[] memory m = new uint256[](2); m[0]=5; m[1]=6; return m[i]; }
    function fixedOOB(uint256 i)   external returns (uint256) { fArr[0]=1; fArr[1]=2; fArr[2]=3; return fArr[i]; }
    function memWriteOOB(uint256 i) external pure returns (uint256) { uint256[] memory m = new uint256[](2); m[i]=7; return m[0]; }
    function mbReadOOB(uint256 i)  external returns (uint256) { mbArr[0]=1; return mbArr[i]; }
    function mbWriteOOB(uint256 i) external returns (uint256) { mbArr[i]=5; return mbArr[0]; }
    function inBounds(uint256 i)   external pure returns (uint256) { uint256[] memory m = new uint256[](3); m[0]=10; m[1]=11; m[2]=12; return m[i]; }
}
