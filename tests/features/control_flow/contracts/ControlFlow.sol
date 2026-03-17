// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Control flow regression test.
contract ControlFlow {
    function ifElse(uint256 x) external pure returns (uint256) {
        if (x > 100) return 3;
        else if (x > 50) return 2;
        else return 1;
    }

    function forLoop(uint256 n) external pure returns (uint256) {
        uint256 sum = 0;
        for (uint256 i = 0; i < n; i++) {
            sum += i;
        }
        return sum;
    }

    function whileLoop(uint256 n) external pure returns (uint256) {
        uint256 sum = 0;
        uint256 i = 0;
        while (i < n) {
            sum += i;
            i++;
        }
        return sum;
    }

    function forWithBreak(uint256 n) external pure returns (uint256) {
        uint256 sum = 0;
        for (uint256 i = 0; i < n; i++) {
            if (i == 5) break;
            sum += i;
        }
        return sum;
    }

    function forWithContinue(uint256 n) external pure returns (uint256) {
        uint256 sum = 0;
        for (uint256 i = 0; i < n; i++) {
            if (i % 2 == 0) continue;
            sum += i;  // only odd numbers
        }
        return sum;
    }

    function ternary(bool cond) external pure returns (uint256) {
        return cond ? 42 : 99;
    }

    function earlyReturn(uint256 x) external pure returns (uint256) {
        if (x == 0) return 0;
        return x * 2;
    }
}
