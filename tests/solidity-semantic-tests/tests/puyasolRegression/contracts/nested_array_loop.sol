// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the differential fuzzer.
// A nested-array extraction evaluated INSIDE a loop condition (`j < a[i].length`) used to
// revert: building the condition emits a bounds-check assert + index cache as
// prePendingStatements, but a WhileLoop condition is a pure expression with no statement
// slot, so they leaked into the loop BODY (after the test that consumes them) → the
// condition read an undefined temp. The for-loop now drains the condition's pre-statements
// and re-runs them each iteration before the test (while(true){pre; if(!cond)break; body}).
contract NestedArrayLoop {
    // double loop over uint256[][] — the condition `j < a[i].length` is the trigger
    function sumNested(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++)
            for (uint j; j < a[i].length; j++) s += a[i][j];
    }
    // a[i].length in the inner-loop condition, no element access in the body
    function countNested(uint256[][] calldata a) external pure returns (uint256 n) {
        for (uint i; i < a.length; i++)
            for (uint j; j < a[i].length; j++) n += 1;
    }
    // break / continue must still route through the for-post under the restructured loop
    function sumEvenIdx(uint256[] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) { if (i % 2 == 1) continue; if (a[i] == 99) break; s += a[i]; }
    }
}
