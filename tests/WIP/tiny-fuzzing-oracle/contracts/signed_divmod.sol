// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // wide dividend / narrow signed divisor (the d9004c0413 fix)
    function divI128by16(int128 a, int16 b) external pure returns (int128) { return a / int128(b); }
    function modI128by16(int128 a, int16 b) external pure returns (int128) { return a % int128(b); }
    function divI256by64(int256 a, int64 b) external pure returns (int256) { return a / int256(b); }
    function modI256by64(int256 a, int64 b) external pure returns (int256) { return a % int256(b); }
    // compound /= with narrow divisor
    function compDiv(int128 a, int16 b) external pure returns (int128) { int128 x = a; x /= int128(b); return x; }
    // mixed signs / extremes via direct same-width
    function divI16(int16 a, int16 b) external pure returns (int16) { return a / b; }
    function modI16(int16 a, int16 b) external pure returns (int16) { return a % b; }
    // unsigned mixed width sanity
    function divU128by8(uint128 a, uint8 b) external pure returns (uint128) { return a / uint128(b); }
    // signed div where divisor widened from negative narrow
    function divNeg(int256 a, int8 b) external pure returns (int256) { return a / int256(b); }
}
