// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. solc-todo.md opportunity A: the "const-fold gap"
// (type(uint64).max**2 reverting on AVM but folding on EVM) turned out NOT to exist — AVM matches EVM:
//   - a constant expression that fits its target is folded to its exact value (10**77, 1<<200, 2^255);
//   - one that overflows its OPERAND type in a checked context reverts on BOTH (type(uint64).max**2 has
//     type uint64, so (2^64-1)^2 overflows -> Panic on EVM and AVM alike — solc does NOT widen it);
//   - unchecked, the same op wraps in its operand width on both (uint64(type(uint64).max**2) == 1).
// This locks that behaviour. tryConstantFold (RationalNumberType + rationalIntConstant) covers the folds;
// the checked overflow is the normal arithmetic path. No ConstantEvaluator integration is needed.
contract ConstFoldArbitraryPrecision {
    function pow1077() external pure returns (uint256) { return 10 ** 77; }                        // fits
    function half256p1() external pure returns (uint256) { return (type(uint256).max / 2) + 1; }   // 2^255
    function bigShift() external pure returns (uint256) { return 1 << 200; }                        // 2^200
    function bigMul() external pure returns (uint256) { return (2 ** 200) * 3; }                     // 3*2^200
    function maxU64sqWrap() external pure returns (uint64) { unchecked { return uint64(type(uint64).max ** 2); } } // 1
    function maxU64sqChecked() external pure returns (uint256) { return type(uint64).max ** 2; }     // REVERTS (uint64 overflow)
}
