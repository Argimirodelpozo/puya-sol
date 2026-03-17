// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Three independent contracts in one file.
/// Each should compile to a separate deployable app.

contract Counter {
    uint256 public count;

    function increment() external {
        count += 1;
    }

    function getCount() external view returns (uint256) {
        return count;
    }
}

contract Storage {
    mapping(bytes32 => uint256) public data;

    function store(bytes32 key, uint256 value) external {
        data[key] = value;
    }

    function load(bytes32 key) external view returns (uint256) {
        return data[key];
    }
}

contract Calculator {
    function add(uint256 a, uint256 b) external pure returns (uint256) {
        return a + b;
    }

    function multiply(uint256 a, uint256 b) external pure returns (uint256) {
        return a * b;
    }

    function power(uint256 base, uint256 exp) external pure returns (uint256) {
        uint256 result = 1;
        for (uint256 i = 0; i < exp; i++) {
            result *= base;
        }
        return result;
    }
}
