// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M16: Inter-app call test.
 * Exercises calling another deployed contract through an interface.
 * On Algorand, interface calls become inner application transactions.
 */
interface ICalculator {
    function add(uint256 a, uint256 b) external pure returns (uint256);
    function multiply(uint256 a, uint256 b) external pure returns (uint256);
    function isPositive(uint256 x) external pure returns (bool);
}

contract InterAppCallTest {
    ICalculator public calculator;

    constructor(address _calculator) {
        calculator = ICalculator(_calculator);
    }

    function callAdd(uint256 a, uint256 b) external view returns (uint256) {
        return calculator.add(a, b);
    }

    function callMultiply(uint256 a, uint256 b) external view returns (uint256) {
        return calculator.multiply(a, b);
    }

    function callIsPositive(uint256 x) external view returns (bool) {
        return calculator.isPositive(x);
    }

    function callAddThenMultiply(uint256 a, uint256 b, uint256 c) external view returns (uint256) {
        uint256 sum = calculator.add(a, b);
        return calculator.multiply(sum, c);
    }
}
