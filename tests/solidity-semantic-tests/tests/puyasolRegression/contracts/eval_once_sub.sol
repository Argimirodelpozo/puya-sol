// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the unsigned-subtraction eval-once fix: a checked `a - f()` must call
// f() exactly once. The pre-fix inlined wrapping-sub referenced the RHS twice
// (the `a >= b` underflow assert AND the (a + 2^256 - b) % 2^256 wrap), so a
// side-effecting f() ran twice.
contract EvalOnceSub {
    uint256 public calls;

    function bump() internal returns (uint256) {
        calls += 1;
        return 1;
    }

    // Checked subtraction with a side-effecting RHS.
    function subOnce(uint256 a) external returns (uint256) {
        return a - bump();
    }
}
