// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // constant operands that overflow the type — EVM folds at arbitrary precision; condition is a const bool
    function cfold(uint64 x) external pure returns (uint64) {
        unchecked {
            if (((type(uint64).max ** 2) + type(uint64).max) >= type(uint64).max) return x + 1;
            return x;
        }
    }
}
