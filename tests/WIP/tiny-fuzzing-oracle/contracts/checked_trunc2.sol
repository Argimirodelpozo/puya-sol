// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract CT2 {
    function inlineCast(uint256 s) external pure returns (uint64) { return uint64(s + 1); }       // bug: wraps
    function viaTemp(uint256 s)    external pure returns (uint64) { uint256 t = s + 1; return uint64(t); } // ?
    function uncheckedInline(uint256 s) external pure returns (uint64) { unchecked { return uint64(s + 1); } } // wrap OK
}
