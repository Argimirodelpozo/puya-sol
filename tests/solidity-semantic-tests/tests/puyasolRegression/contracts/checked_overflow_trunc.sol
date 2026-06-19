// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the differential fuzzer.
// A checked uint256 op whose result is immediately narrowed (uintN(s + 1)) must STILL revert on
// the uint256 overflow — the overflow is checked at full width before truncation, not wrapped.
contract CheckedOverflowTrunc {
    function truncAdd(uint256 s) external pure returns (uint64) { return uint64(s + 1); }
    function truncMul(uint256 s) external pure returns (uint64) { return uint64(s * 2); }
    function viaTemp(uint256 s)  external pure returns (uint64) { uint256 t = s + 1; return uint64(t); }
    function uncheckedOK(uint256 s) external pure returns (uint64) { unchecked { return uint64(s + 1); } }
    function normalAdd(uint256 a, uint256 b) external pure returns (uint64) { return uint64(a + b); }
}
