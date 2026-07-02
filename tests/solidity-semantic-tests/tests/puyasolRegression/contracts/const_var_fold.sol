// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Guard for SolcConstFold::foldTyped (fable-review item 1, case (b)): intN-typed expressions
// over CONSTANT VARIABLES fold via solc's ConstantEvaluator ONLY when every integer-typed node
// in the subtree evaluates in range of its OWN type. solc's TypeChecker already hard-errors
// CHECKED binary constant overflow at compile time, but lets UNARY negation overflow and
// SHIFT/unchecked-mul truncation through to runtime — exactly where the guard is load-bearing:
// an out-of-range intermediate must reach the runtime lowering (revert / wrap), never be
// swallowed by rational math.
contract C {
    int8 constant M = type(int8).min;   // -128
    int8 constant P = 100;
    int256 constant A = 7;
    int256 constant B = 3;
    int128 constant W = -12345678901234567890;
    uint256 constant BIG = 2 ** 200;

    // value 128 is out of int8 range -> NO fold -> checked negate REVERTS (the -M trap)
    function negMin() external pure returns (int8) { return -M; }
    // P<<1 = 200 out of int8 range: runtime truncates to -56, then -56>>1 = -28.
    // (a wrong rational fold would give (200>>1) = 100)
    function shiftTrunc() external pure returns (int8) { return (P << 1) >> 1; }
    // unchecked wrap participates: int8(300) = 44 (a wrong fold would need 300 in-range)
    function mulWrapUnchecked() external pure returns (int8) { unchecked { return P * 3; } }
    // happy folds (identical values via the runtime path if the evaluator declines)
    function divTrunc() external pure returns (int256) { return (-A) / B; }        // -2, toward zero
    function modSign() external pure returns (int256) { return (-A) % B; }         // -1, sign of dividend
    function arith() external pure returns (int256) { return (A * B + 1) << 2; }   // 88
    function wide() external pure returns (int128) { return -W; }                  // biguint-backed width
    function big() external pure returns (uint256) { return BIG / 2 + 1; }
}
