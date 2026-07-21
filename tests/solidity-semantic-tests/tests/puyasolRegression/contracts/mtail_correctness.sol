// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards a batch of medium-tail correctness fixes (fable-review-3):
// M1  tuple destructuring applies per-element coercion (signed widen);
// M18 overriding an overloaded base method must not re-emit the base body;
// M20 `.selector` on a ternary evaluates the condition once;
// M24 mulmod/addmod evaluate x/y before the modulus zero-check.

contract MTailBase {
    function f(uint256 x) public pure virtual returns (uint256) {
        return x + 1;
    }
}

contract MTailCorrectness is MTailBase {
    uint256 public cnt;

    // M18: overriding an OVERLOADED base method must not re-emit the base body.
    function f(uint256 x) public pure override returns (uint256) {
        return x + 100;
    }

    function f(uint256 x, uint256 y) public pure returns (uint256) {
        return x + y;
    }

    // Distinct single-signature externals for the .selector ternary (this.f
    // would be ambiguous across the overloads).
    function ga() external pure returns (uint256) { return 1; }
    function gb() external pure returns (uint256) { return 2; }

    // M1: signed sub-word element must sign-extend into the wider decl.
    function tupleSignedWiden() external pure returns (int256, uint256) {
        int8 s = -1;
        (int128 a, uint256 b) = (s, 2);
        return (a, b); // a must be -1, not +255
    }

    function bump() internal returns (bool) {
        cnt += 1;
        return true;
    }

    // M20: `.selector` on a ternary — the condition runs exactly once.
    function selectorTernary() external returns (uint256) {
        cnt = 0;
        bytes4 sel = (bump() ? this.ga : this.gb).selector;
        sel; // used
        return cnt; // must be 1, not 2
    }

    function preBump() internal returns (uint256) {
        cnt += 1;
        return 6;
    }

    // M24: mulmod evaluates x (which bumps) exactly once.
    function mulmodOrder(uint256 y) external returns (uint256 r, uint256 n) {
        cnt = 0;
        r = mulmod(preBump(), y, 7);
        n = cnt; // preBump ran exactly once
    }
}
