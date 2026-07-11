// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function exp(uint64 a, uint64 e)  external pure returns (uint64) { unchecked { return a ** e; } }   // overflow wraps
    function expc(uint64 a, uint64 e) external pure returns (uint64) { return a ** e; }                  // checked: reverts
    function exp32(uint32 a, uint32 e) external pure returns (uint32) { unchecked { return a ** e; } }   // sub-word (handled)
}
