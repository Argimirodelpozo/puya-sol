// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract Overloading {
    // Same name, different parameter counts
    function compute(uint256 a) external pure returns (uint256) {
        return a * 2;
    }

    function compute(uint256 a, uint256 b) external pure returns (uint256) {
        return a + b;
    }

    function compute(uint256 a, uint256 b, uint256 c) external pure returns (uint256) {
        return a + b + c;
    }

    // Same name, different parameter types
    function convert(bool val) external pure returns (uint256) {
        return val ? 1 : 0;
    }

    function convert(uint256 val) external pure returns (bool) {
        return val != 0;
    }

    // Overloaded with address
    function identify(address a) external pure returns (uint256) {
        return 1; // address variant
    }

    function identify(uint256 a) external pure returns (uint256) {
        return 2; // uint256 variant
    }

    // Internal overloaded helper
    function doubleIt(uint256 x) internal pure returns (uint256) {
        return x * 2;
    }

    function doubleIt(uint256 x, uint256 y) internal pure returns (uint256) {
        return (x + y) * 2;
    }

    function useInternal1(uint256 x) external pure returns (uint256) {
        return doubleIt(x);
    }

    function useInternal2(uint256 x, uint256 y) external pure returns (uint256) {
        return doubleIt(x, y);
    }
}
