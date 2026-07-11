// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A {
    function u256(uint256 a) external pure returns (uint256 r) { assembly { r := add(a, 1) } }         // wraps at 2^256
    function u128(uint256 a) external pure returns (uint128 r) { assembly { r := add(a, 1) } }         // unchecked → %2^128
    function u64(uint256 a) external pure returns (uint64 r) { assembly { r := a } }
    function u128b(uint256 a, uint256 b) external pure returns (uint128 r) { assembly { r := mul(a, b) } } // overflow → wrap
}
