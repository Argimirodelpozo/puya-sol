// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function subU(uint64 a, uint64 b)   external pure returns (uint64) { unchecked { return a - b; } }  // underflow wraps
    function subUc(uint64 a, uint64 b)  external pure returns (uint64) { return a - b; }                 // checked: reverts
    function subU32(uint32 a, uint32 b) external pure returns (uint32) { unchecked { return a - b; } }   // sub-word (handled)
    function subU256(uint256 a, uint256 b) external pure returns (uint256) { unchecked { return a - b; } } // biguint (handled)
}
