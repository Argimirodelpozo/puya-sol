// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M41: Callee for struct return from inner calls (Gap 6 verification).
 */

struct Point {
    uint256 x;
    uint256 y;
}

interface IStructCallee {
    function getPoint(uint256 px, uint256 py) external pure returns (uint256, uint256);
    function getSum(uint256 a, uint256 b) external pure returns (uint256);
}

contract StructCallee {
    function getPoint(uint256 px, uint256 py) external pure returns (uint256, uint256) {
        return (px * 2, py * 3);
    }

    function getSum(uint256 a, uint256 b) external pure returns (uint256) {
        return a + b;
    }
}
