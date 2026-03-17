// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/FixedPointMathLib.sol";

contract FixedPointMathWrapper {
    function mulWad(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.mulWad(x, y);
    }

    function divWad(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.divWad(x, y);
    }

    function mulWadUp(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.mulWadUp(x, y);
    }

    function divWadUp(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.divWadUp(x, y);
    }

    function mulDiv(uint256 x, uint256 y, uint256 d) external pure returns (uint256) {
        return FixedPointMathLib.mulDiv(x, y, d);
    }

    function sqrt(uint256 x) external pure returns (uint256) {
        return FixedPointMathLib.sqrt(x);
    }

    function cbrt(uint256 x) external pure returns (uint256) {
        return FixedPointMathLib.cbrt(x);
    }

    function log2(uint256 x) external pure returns (uint256) {
        return FixedPointMathLib.log2(x);
    }

    function log2Up(uint256 x) external pure returns (uint256) {
        return FixedPointMathLib.log2Up(x);
    }

    function log10(uint256 x) external pure returns (uint256) {
        return FixedPointMathLib.log10(x);
    }

    function abs(int256 x) external pure returns (uint256) {
        return FixedPointMathLib.abs(x);
    }

    function dist(int256 x, int256 y) external pure returns (uint256) {
        return FixedPointMathLib.dist(x, y);
    }

    function min(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.min(x, y);
    }

    function max(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.max(x, y);
    }

    function clamp(uint256 x, uint256 minVal, uint256 maxVal) external pure returns (uint256) {
        return FixedPointMathLib.clamp(x, minVal, maxVal);
    }

    function rawAdd(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.rawAdd(x, y);
    }

    function rawSub(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.rawSub(x, y);
    }

    function rawMul(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.rawMul(x, y);
    }

    function rawDiv(uint256 x, uint256 y) external pure returns (uint256) {
        return FixedPointMathLib.rawDiv(x, y);
    }
}
