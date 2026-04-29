// SPDX-License-Identifier: MIT
pragma solidity >=0.8.19;

import "./ud60x18/Math.sol" as UD60x18Math;
import "./ud60x18/Casting.sol" as UD60x18Casting;
import "./Common.sol" as Common;
import { UD60x18 } from "./ud60x18/ValueType.sol";

/// @title UD60x18Wrapper
/// @notice Thin wrapper to expose PRBMath UD60x18 free functions as contract methods for testing.
contract UD60x18Wrapper {

    function avg(uint256 x, uint256 y) external pure returns (uint256) {
        return UD60x18Math.avg(UD60x18.wrap(x), UD60x18.wrap(y)).unwrap();
    }

    function ceil(uint256 x) external pure returns (uint256) {
        return UD60x18Math.ceil(UD60x18.wrap(x)).unwrap();
    }

    function div(uint256 x, uint256 y) external pure returns (uint256) {
        return UD60x18Math.div(UD60x18.wrap(x), UD60x18.wrap(y)).unwrap();
    }

    function exp(uint256 x) external pure returns (uint256) {
        return UD60x18Math.exp(UD60x18.wrap(x)).unwrap();
    }

    function exp2(uint256 x) external pure returns (uint256) {
        return UD60x18Math.exp2(UD60x18.wrap(x)).unwrap();
    }

    function floor(uint256 x) external pure returns (uint256) {
        return UD60x18Math.floor(UD60x18.wrap(x)).unwrap();
    }

    function frac(uint256 x) external pure returns (uint256) {
        return UD60x18Math.frac(UD60x18.wrap(x)).unwrap();
    }

    function gm(uint256 x, uint256 y) external pure returns (uint256) {
        return UD60x18Math.gm(UD60x18.wrap(x), UD60x18.wrap(y)).unwrap();
    }

    function inv(uint256 x) external pure returns (uint256) {
        return UD60x18Math.inv(UD60x18.wrap(x)).unwrap();
    }

    function ln(uint256 x) external pure returns (uint256) {
        return UD60x18Math.ln(UD60x18.wrap(x)).unwrap();
    }

    function log2(uint256 x) external pure returns (uint256) {
        return UD60x18Math.log2(UD60x18.wrap(x)).unwrap();
    }

    function log10(uint256 x) external pure returns (uint256) {
        return UD60x18Math.log10(UD60x18.wrap(x)).unwrap();
    }

    function mul(uint256 x, uint256 y) external pure returns (uint256) {
        return UD60x18Math.mul(UD60x18.wrap(x), UD60x18.wrap(y)).unwrap();
    }

    function pow(uint256 x, uint256 y) external pure returns (uint256) {
        return UD60x18Math.pow(UD60x18.wrap(x), UD60x18.wrap(y)).unwrap();
    }

    function powu(uint256 x, uint256 y) external pure returns (uint256) {
        return UD60x18Math.powu(UD60x18.wrap(x), y).unwrap();
    }

    function sqrt(uint256 x) external pure returns (uint256) {
        return UD60x18Math.sqrt(UD60x18.wrap(x)).unwrap();
    }

    // Common.sol functions
    function mulDiv(uint256 x, uint256 y, uint256 denominator) external pure returns (uint256) {
        return Common.mulDiv(x, y, denominator);
    }

    function mulDiv18(uint256 x, uint256 y) external pure returns (uint256) {
        return Common.mulDiv18(x, y);
    }

    function commonSqrt(uint256 x) external pure returns (uint256) {
        return Common.sqrt(x);
    }

    function commonExp2(uint256 x) external pure returns (uint256) {
        return Common.exp2(x);
    }

    function msb(uint256 x) external pure returns (uint256) {
        return Common.msb(x);
    }
}
