// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found while fixing the signed-mul finding.
// The RHS of a short-circuit && / || that has SIDE EFFECTS (a checked op's overflow/zero assert, a `**`
// loop, ...) was lowered with those side effects HOISTED to the enclosing statement, so they ran
// unconditionally. EVM short-circuits: the classic guard idiom `b != 0 && a / b > x` must NOT divide when
// b == 0, and `(b == 0) || (a / b == 0)` must NOT revert when b == 0. FIX (SolBinaryOperation
// trySolShortCircuit): capture the RHS's pre-statements and gate them behind the condition via an
// if/else (a && b == a ? b : false; a || b == a ? true : b), mirroring the ternary. The side effect still
// runs when the branch IS taken (no over-suppression); plain &&/|| with no RHS side effects are unchanged.
contract ShortCircuitRhsSideEffects {
    function orDiv(int64 a, int64 b)  external pure returns (bool) { return (b == 0) || ((a / b) == 0); }   // b==0 -> true (was revert)
    function andDiv(int64 a, int64 b) external pure returns (bool) { return (b != 0) && ((a / b) > 5); }    // b==0 -> false (was revert)
    function orNeg(int64 a, int64 c)  external pure returns (bool) { return (c == 0) || ((-a) == 0); }      // c==0 -> true (was revert at a=min)
    function orAdd(int64 a, int64 b, int64 c) external pure returns (bool) { return (c == 0) || ((a + b) == 0); }
    function nested(int64 a, int64 b, int64 c) external pure returns (bool) { return (c == 0) || ((b == 0) || ((a / b) == 0)); }
    // side effect STILL fires when the branch IS taken (no over-suppression): a+1 overflows at int64.max
    function rhsTaken(int64 a, int64 c) external pure returns (bool) { return (c != 0) && ((a + 1) > a); }
    // controls: plain &&/|| with no RHS side effects must be unchanged
    function cmpAnd(uint64 a, uint64 b) external pure returns (bool) { return (a > 3) && (b < 10); }
    function plainOr(bool x, bool y)    external pure returns (bool) { return x || y; }
}
