// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function decU256Unchk(uint256 a) external pure returns (uint256) { unchecked { uint256 x=a; x--; return x; } }
    function decU128Unchk(uint128 a) external pure returns (uint128) { unchecked { uint128 x=a; x--; return x; } }
    function decU8Unchk(uint8 a) external pure returns (uint8) { unchecked { uint8 x=a; x--; return x; } }
}
