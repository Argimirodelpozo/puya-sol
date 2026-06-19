// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test.
// Solidity passes memory by reference: an internal method that mutates a memory struct param
// writes through to the caller. A read-only param must NOT trigger spurious write-back.
contract MemStructParam {
    struct S { uint256 x; uint256 y; }
    function _mut(S memory s) internal pure { s.x = 11; }
    function _readonly(S memory s) internal pure returns (uint256) { return s.x; }
    function writesThrough() external pure returns (uint256) {
        S memory s = S(5, 0); _mut(s); return s.x;     // EVM: 11
    }
    function readonlyUnchanged() external pure returns (uint256) {
        S memory s = S(7, 0); uint256 r = _readonly(s); return r + s.x;  // EVM: 14 (no write-back)
    }
}
