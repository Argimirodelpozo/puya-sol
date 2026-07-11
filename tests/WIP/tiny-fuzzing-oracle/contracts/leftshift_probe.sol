// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Left shift (checked overflow + unchecked wrap), sub-word + full, signed + unsigned.
contract LeftShift {
    function shl8c(uint8 x, uint256 n)   external pure returns (uint8)   { return x << n; }              // checked
    function shl8u(uint8 x, uint256 n)   external pure returns (uint8)   { unchecked { return x << n; } }// wrap
    function shlI8u(int8 x, uint256 n)   external pure returns (int8)    { unchecked { return x << n; } }// signed wrap
    function shl256u(uint256 x, uint256 n) external pure returns (uint256) { unchecked { return x << n; } }
    function shlI256(int256 x, uint256 n) external pure returns (int256) { unchecked { return x << n; } }
}
