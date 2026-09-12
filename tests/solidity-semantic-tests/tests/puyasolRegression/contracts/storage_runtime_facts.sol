// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract StorageRuntimeFacts {
    uint256 private value;
    uint256 transient scratch;
    modifier unused() { assembly { sstore(0x12340000, 99) } _; }
    function unusedFunction() private { assembly { sstore(0x12340000, 88) } }
    function update(uint256 next) external returns (uint256) {
        value = next;
        scratch = next + 1;
        return value + scratch;
    }
    function read() external view returns (uint256, uint256) { return (value, scratch); }
}
