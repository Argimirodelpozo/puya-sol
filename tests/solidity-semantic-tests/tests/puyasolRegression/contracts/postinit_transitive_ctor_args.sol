// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards __postInit's base-ctor-arg ordering: for D is C is A with
// `C(uint y) A(y + 1)`, A's arg reads C's param y, so C's params must be
// assigned before A's args are evaluated (derived-first). The pre-fix postInit
// loop went base-most-first and interleaved args with bodies: `x = y + 1` ran
// before `y = 5`, so va ended up 1 instead of 6. The box array (uint[] state
// var) forces the __postInit deploy path; the non-postInit path had the fix.
contract A {
    uint256 public va;
    constructor(uint256 x) {
        va = x;
    }
}

contract C is A {
    uint256 public y2;
    constructor(uint256 y) A(y + 1) {
        y2 = y;
    }
}

contract D is C {
    uint256[] public arr;
    constructor() C(5) {
        arr.push(9);
    }
}
