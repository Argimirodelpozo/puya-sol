// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M40: Callee contract for cross-contract return value test (Gap 1).
 * Returns values from functions that the caller will read via .call().
 */

interface IReturnCallee {
    function getNumber() external view returns (uint256);
    function add(uint256 a, uint256 b) external pure returns (uint256);
    function isEven(uint256 n) external pure returns (bool);
}

contract ReturnCallee {
    uint256 public storedValue;

    constructor() {
        storedValue = 42;
    }

    function getNumber() external view returns (uint256) {
        return storedValue;
    }

    function add(uint256 a, uint256 b) external pure returns (uint256) {
        return a + b;
    }

    function isEven(uint256 n) external pure returns (bool) {
        return n % 2 == 0;
    }

    function setNumber(uint256 val) external {
        storedValue = val;
    }
}
