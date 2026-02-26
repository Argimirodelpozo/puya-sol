// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

library MathLib {
    function square(uint256 x) internal pure returns (uint256) {
        return x * x;
    }

    function clamp(uint256 val, uint256 lo, uint256 hi) internal pure returns (uint256) {
        if (val < lo) return lo;
        if (val > hi) return hi;
        return val;
    }
}

contract MathLibTest {
    function testSquare(uint256 x) external pure returns (uint256) {
        return MathLib.square(x);
    }

    function testClamp(uint256 v, uint256 lo, uint256 hi) external pure returns (uint256) {
        return MathLib.clamp(v, lo, hi);
    }
}
