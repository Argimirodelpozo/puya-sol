// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract BR {
    function ifElem(bool[] calldata f, uint256 i) external pure returns (uint256) { if (f[i]) return 1; return 0; }
    function ternElem(bool[] calldata f, uint256 i) external pure returns (uint256) { return f[i] ? 7 : 9; }
    function reqElem(bool[] calldata f, uint256 i) external pure returns (uint256) { require(f[i]); return 3; }
    function andElem(bool[] calldata f, uint256 i, uint256 j) external pure returns (bool) { return f[i] && f[j]; }
    function retElem(bool[] calldata f, uint256 i) external pure returns (bool) { return f[i]; }
}
