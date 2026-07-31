// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
import {TickMath} from "./TickMath.sol";
import {FullMath} from "./FullMath.sol";
// Wrapper exposing Uniswap V4 TickMath + FullMath for differential fuzzing.
contract UniMathWrapper {
    function getSqrtPriceAtTick(int24 tick) external pure returns (uint160) { return TickMath.getSqrtPriceAtTick(tick); }
    function getTickAtSqrtPrice(uint160 p) external pure returns (int24) { return TickMath.getTickAtSqrtPrice(p); }
    function mulDiv(uint256 a, uint256 b, uint256 d) external pure returns (uint256) { return FullMath.mulDiv(a, b, d); }
    function mulDivRoundingUp(uint256 a, uint256 b, uint256 d) external pure returns (uint256) { return FullMath.mulDivRoundingUp(a, b, d); }
}
