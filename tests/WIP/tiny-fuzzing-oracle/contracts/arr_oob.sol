// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract ArrOOB {
    uint256[] sArr;
    uint256[3] fArr;
    function memOOB(uint256 i)   external pure returns (uint256) { uint256[] memory m = new uint256[](2); m[0]=5; m[1]=6; return m[i]; }
    function fixedOOB(uint256 i) external returns (uint256)      { fArr[0]=1; fArr[1]=2; fArr[2]=3; return fArr[i]; }
    function storageOOB(uint256 i) external returns (uint256)    { delete sArr; sArr.push(9); sArr.push(8); return sArr[i]; }
    function memWriteOOB(uint256 i) external pure returns (uint256) { uint256[] memory m = new uint256[](2); m[i]=7; return m[0]; }
}
