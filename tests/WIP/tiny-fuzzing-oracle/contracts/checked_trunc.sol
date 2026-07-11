// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract CheckedTrunc {
    function truncAdd(uint256 s)  external pure returns (uint64)  { return uint64(s + 1); }     // s=2^256-1 -> EVM reverts
    function plainAdd(uint256 s)  external pure returns (uint256) { return s + 1; }              // control: must revert
    function truncMul(uint256 s)  external pure returns (uint64)  { return uint64(s * 2); }
    function truncAddVar(uint256 s, uint256 k) external pure returns (uint64) { return uint64(s + k); }
}
