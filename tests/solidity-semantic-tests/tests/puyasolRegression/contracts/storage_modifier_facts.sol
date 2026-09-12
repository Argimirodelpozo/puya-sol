// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract StorageModifierFacts {
    uint256 private value;
    modifier writeValue() { assembly { sstore(value.slot, 77) } _; }
    function read() external writeValue returns (uint256) { return value; }
}
