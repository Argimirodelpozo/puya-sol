// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // short-circuit guarding a reverting RHS (div-by-zero / overflow)
    function scDiv(uint256 a, uint256 b, uint256 t) external pure returns (bool) {
        return (b != 0) && (a / b > t);
    }
    function scMod(int256 a, int256 b) external pure returns (bool) {
        return (b != 0) || (a % b == 0);   // RHS only when b==0 -> must revert
    }
    // ternary with signed-mul operands (complex left operand)
    function ternMul(int128 a, int128 b, bool c) external pure returns (int128) {
        return (c ? (a << 1) : (a >> 1)) * b;
    }
    // typemin negate (checked) — must revert at type.min
    function negMin(int64 a) external pure returns (int64) {
        return -a;
    }
    function negMin32(int32 a) external pure returns (int32) {
        return -a;
    }
    // ternary-in-condition with reverting branch
    function chain(uint128 a, uint128 b) external pure returns (uint128) {
        uint128 r = (a > b ? a - b : b - a);       // checked sub, never underflows
        return (r != 0) ? (a * b) / r : a + b;     // checked mul/div/add
    }
    // signed mul where left is a cast (SingleEvaluation materialization)
    function castMul(int256 x, int16 y) external pure returns (int256) {
        return int256(int16(x)) * int256(y);
    }
}
