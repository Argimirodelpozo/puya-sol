// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function lit3u(uint64 a)  external pure returns (uint64) { unchecked { return a ** 3; } }   // literal exp, unchecked
    function lit2u(uint64 a)  external pure returns (uint64) { unchecked { return a ** 2; } }
    function lit3c(uint64 a)  external pure returns (uint64) { return a ** 3; }                  // checked: overflow reverts
    function varu(uint64 a, uint64 e) external pure returns (uint64) { unchecked { return a ** e; } } // variable exp
}
