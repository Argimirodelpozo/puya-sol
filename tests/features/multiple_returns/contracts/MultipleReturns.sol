// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract MultipleReturns {
    function twoValues() external pure returns (uint256, bool) {
        return (42, true);
    }

    function threeValues() external pure returns (uint256, uint256, uint256) {
        return (1, 2, 3);
    }

    function namedReturns() external pure returns (uint256 x, uint256 y) {
        x = 10;
        y = 20;
    }

    function conditionalReturn(bool flag) external pure returns (uint256, bool) {
        if (flag) return (100, true);
        return (0, false);
    }

    function returnWithComputation(uint256 a, uint256 b) external pure returns (uint256 sum, uint256 product) {
        sum = a + b;
        product = a * b;
    }

    // Use multiple return values from internal function
    function useInternal(uint256 x) external pure returns (uint256) {
        (uint256 a, uint256 b) = computePair(x);
        return a + b;
    }

    function computePair(uint256 x) internal pure returns (uint256, uint256) {
        return (x * 2, x * 3);
    }
}
