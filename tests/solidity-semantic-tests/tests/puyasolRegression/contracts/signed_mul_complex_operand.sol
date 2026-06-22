// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--cast).
// A complex (non-leaf) expression used as the LEFT operand of a checked SIGNED multiply false-reverted:
// `(bitwise/shift/cast-chain/ternary) * x` REVERTED on the AVM where EVM returns the value (most visibly
// at x == 0, where the product is 0). Root: SolBinaryOperation makeEvalOnce's the operand into a
// SingleEvaluation, and puya mis-lowers SingleEvaluation(complex) in the signed-mul abs/overflow codegen
// (stack-slot miscount). Add/sub and a complex RIGHT operand were unaffected. FIX: materialise a complex
// (SingleEvaluation) left operand of a signed multiply to a REAL local first (an explicit
// `T t = expr; t * x` was always clean). The return path masks, so it only surfaced mid-expression.
contract SignedMulComplexOperand {
    function ternMul(int16 a, int16 b, int16 c) external pure returns (int16) { return ((c < b) ? c : a) * a; }
    function andMul(int64 a, int64 b, int64 c)  external pure returns (int64) { return (a & b) * c; }
    function notMul(int64 a, int64 c)           external pure returns (int64) { return (~a) * c; }
    function shlMul(int64 a, int64 c)           external pure returns (int64) { return (a << 1) * c; }
    function castMul(int64 a, int64 b)          external pure returns (int64) { return int64(int8(a)) * b; }
    function inRange(int64 a, int64 b)          external pure returns (int64) { return (a & b) * 3; }
    // pure-left short-circuit must stay clean (operand never reverts, so eager materialise is harmless)
    function scAnd(int64 a, int64 b, int64 c)   external pure returns (bool) { return (c == 0) || (((a & b) * c) == 0); }
    // a real overflow must STILL revert (fix only removes the false revert)
    function overflowMul(int64 a, int64 b)      external pure returns (int64) { return (a | b) * (a | b); }
}
