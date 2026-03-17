// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Arithmetic operations regression test.
contract Arithmetic {
    function add(uint256 a, uint256 b) external pure returns (uint256) { return a + b; }
    function sub(uint256 a, uint256 b) external pure returns (uint256) { return a - b; }
    function mul(uint256 a, uint256 b) external pure returns (uint256) { return a * b; }
    function div(uint256 a, uint256 b) external pure returns (uint256) { return a / b; }
    function mod(uint256 a, uint256 b) external pure returns (uint256) { return a % b; }

    function addUnchecked(uint256 a, uint256 b) external pure returns (uint256) {
        unchecked { return a + b; }
    }

    function subUnchecked(uint256 a, uint256 b) external pure returns (uint256) {
        unchecked { return a - b; }
    }

    // Signed arithmetic (unchecked to allow two's complement wrapping)
    function addSigned(uint256 a, uint256 b) external pure returns (uint256) {
        unchecked { return a + b; }
    }
    function subSigned(uint256 a, uint256 b) external pure returns (uint256) {
        unchecked { return a - b; }
    }
    function negateSigned(uint256 a) external pure returns (uint256) {
        unchecked { return 0 - a; }  // two's complement: ~a + 1
    }

    // Increment/decrement
    uint256 public counter;

    function increment() external returns (uint256) {
        counter++;
        return counter;
    }

    function decrement() external returns (uint256) {
        counter--;
        return counter;
    }

    function preIncrement() external returns (uint256) {
        return ++counter;
    }

    function resetCounter() external {
        counter = 0;
    }
}
