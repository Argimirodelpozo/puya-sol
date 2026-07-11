// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // internal callee with narrow signed param/return, called from wide context
    function addI16(int16 a, int16 b) internal pure returns (int16) { unchecked { return a + b; } }
    function viaI16(int128 x, int128 y) external pure returns (int128) {
        return int128(addI16(int16(x), int16(y)));
    }
    function mulU128(uint128 a, uint128 b) internal pure returns (uint128) { unchecked { return a * b; } }
    function viaU128(uint256 x, uint256 y) external pure returns (uint256) {
        return uint256(mulU128(uint128(x), uint128(y)));
    }
    // signed mixed-width compare chain
    function cmpMix(int8 a, int64 b, int128 c) external pure returns (bool) {
        return (int128(a) < c) && (int64(a) <= b) && (b > int64(int8(a)));
    }
    // narrow signed return widened then compared
    function subWiden(int32 a, int32 b) internal pure returns (int32) { unchecked { return a - b; } }
    function viaSub(int256 x, int256 y) external pure returns (int256) {
        int32 r = subWiden(int32(x), int32(y));
        return int256(r) * 2;
    }
    // unsigned narrow callee returning into signed context
    function shr(uint64 a, uint64 n) internal pure returns (uint64) { return a >> n; }
    function viaShr(uint256 x, uint256 n) external pure returns (uint256) {
        return uint256(shr(uint64(x), uint64(n % 64)));
    }
}
