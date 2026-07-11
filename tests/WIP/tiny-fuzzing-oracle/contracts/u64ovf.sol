// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function addu(uint64 a, uint64 b) external pure returns (uint64) { unchecked { return a + b; } }  // overflow wraps?
    function mulu(uint64 a, uint64 b) external pure returns (uint64) { unchecked { return a * b; } }
    function powu(uint64 a)           external pure returns (uint64) { unchecked { return a ** 2; } }
}
