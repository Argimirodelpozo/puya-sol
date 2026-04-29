// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

contract NestedMapTest {
    // Simulates nullifier tracking: scope => id => used
    mapping(uint256 => mapping(uint256 => bool)) public nullifiers;

    function setNullifier(uint256 scope, uint256 id) external {
        nullifiers[scope][id] = true;
    }

    function clearNullifier(uint256 scope, uint256 id) external {
        nullifiers[scope][id] = false;
    }

    function isNullified(uint256 scope, uint256 id) external view returns (bool) {
        return nullifiers[scope][id];
    }
}
