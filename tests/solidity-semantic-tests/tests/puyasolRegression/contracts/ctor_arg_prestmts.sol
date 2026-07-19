// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the base-ctor-argument pre-statement drain: a ternary (branch-lowered)
// ctor argument builds as a temp read whose assigning if/else lands in the
// builder's pre-pending buffer. The pre-fix create path emitted `x = __cond_N`
// BEFORE the if/else that assigns __cond_N, so A saw 0 instead of |a|.
contract A {
    int256 public va;
    constructor(int256 x) {
        va = x;
    }
}

contract D is A {
    constructor(int256 a) A(a > 0 ? a : -a) {}
}
