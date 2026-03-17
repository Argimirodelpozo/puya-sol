// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

library MathLib {
    function add(uint256 a, uint256 b) internal pure returns (uint256) {
        return a + b;
    }

    function mul(uint256 a, uint256 b) internal pure returns (uint256) {
        return a * b;
    }

    function clamp(uint256 val, uint256 minVal, uint256 maxVal) internal pure returns (uint256) {
        if (val < minVal) return minVal;
        if (val > maxVal) return maxVal;
        return val;
    }
}

contract Libraries {
    using MathLib for uint256;

    function testAdd(uint256 a, uint256 b) external pure returns (uint256) {
        return a.add(b);
    }

    function testMul(uint256 a, uint256 b) external pure returns (uint256) {
        return a.mul(b);
    }

    function testClamp(uint256 val, uint256 lo, uint256 hi) external pure returns (uint256) {
        return val.clamp(lo, hi);
    }

    // Direct library call
    function testDirect(uint256 a, uint256 b) external pure returns (uint256) {
        return MathLib.add(a, b);
    }
}
