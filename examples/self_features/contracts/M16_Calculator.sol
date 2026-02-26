// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M16 callee: Simple calculator contract deployed as a separate app.
 */
contract Calculator {
    function add(uint256 a, uint256 b) external pure returns (uint256) {
        return a + b;
    }

    function multiply(uint256 a, uint256 b) external pure returns (uint256) {
        return a * b;
    }

    function isPositive(uint256 x) external pure returns (bool) {
        return x > 0;
    }
}
