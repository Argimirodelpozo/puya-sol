// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
// Isolates exp() constant-fold in SCALAR asm (no memory pointers).
contract ExpScalar {
    function shl16(uint256 a) external pure returns (uint256 r) {
        assembly { r := mul(a, exp(256, 2)) }   // a << 16
    }
    function shr16(uint256 a) external pure returns (uint256 r) {
        assembly { r := div(a, exp(256, 2)) }    // a >> 16
    }
    function shl96(uint256 a) external pure returns (uint256 r) {
        assembly { r := mul(a, exp(256, 12)) }   // a << 96  (the AddrResolver constant)
    }
    function roundtrip(uint256 a) external pure returns (uint256 r) {
        assembly { r := div(mul(a, exp(256, 12)), exp(256, 12)) }  // a
    }
}
