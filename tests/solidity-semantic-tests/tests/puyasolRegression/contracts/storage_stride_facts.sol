// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract StorageStrideFacts {
    uint8[2**38][2] private values;
    function set(uint256 i, uint256 j, uint8 value) external returns (uint8) {
        values[i][j] = value;
        return values[i][j];
    }
    function get(uint256 i, uint256 j) external view returns (uint8) {
        return values[i][j];
    }
}
