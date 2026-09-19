// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

contract DeferredChildBase {
    uint256[] private unusedArray;
    uint256 public value = 7;
}
contract DeferredChild is DeferredChildBase {
    uint256 public immutable immutableValue;
    constructor() { value = 9; immutableValue = 11; }
}
contract DeferredMappingChild {
    mapping(uint256 => uint256) private unusedMapping;
    uint256 public value = 13;
}
contract ChildInitialization {
    function createChild() external returns (uint256, uint256) {
        DeferredChild child = new DeferredChild();
        return (child.value(), child.immutableValue());
    }
    function createMappingChild() external returns (uint256) {
        return (new DeferredMappingChild()).value();
    }
}
