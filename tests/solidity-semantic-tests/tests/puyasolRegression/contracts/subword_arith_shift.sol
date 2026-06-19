// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the differential fuzzer.
// Arithmetic shift right of a SIGNED sub-word int by a (dynamic) amount >= its width must
// SIGN-fill: int8(-1) >> 256 == -1 (not 0), positive >> 256 == 0.
contract SubwordArithShift {
    function shr8(int8 x, uint256 n)   external pure returns (int8)  { return x >> n; }
    function shr16(int16 x, uint256 n) external pure returns (int16) { return x >> n; }
}
