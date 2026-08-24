// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

// Transient inc/dec must share the general lvalue path's width/sign/checked
// rules and write placement. The old early-out computed a bare ±1 (uint8 at
// 255 ++'d to a silent wrap where solc panics) and pushed the postfix write to
// POST-effects, so a later read in the same STATEMENT saw the stale value
// (legacy solc writes immediately: look(t++, t) reads the updated t for the
// second argument). Expected values below verified against solc 0.8.28
// LEGACY codegen on py-evm — the project's oracle (NOT via-IR, which orders
// binary operands left-first and would give 11/12 for intra/pre).
contract TransientIncDec {
    uint256 transient t;
    uint8 transient t8;

    function look(uint256 a, uint256 b) internal pure returns (uint256) {
        return a * 100 + b;
    }

    // Postfix write lands before the SECOND argument evaluates: 5*100 + 6.
    function callArgs() external returns (uint256 r) {
        t = 5;
        r = look(t++, t);
    }

    // Legacy solc evaluates the RIGHT operand first: 5 + 5.
    function intra() external returns (uint256 r) {
        t = 5;
        r = t++ + t;
    }

    // Right first (5), then ++t (6): 11.
    function pre() external returns (uint256 r) {
        t = 5;
        r = ++t + t;
    }

    function chained() external returns (uint256 r) {
        t = 5;
        t++;
        t++;
        r = t;
    }

    // Checked sub-word inc at the type max must revert (Panic on EVM), not wrap.
    function overflowInc() external returns (uint256 r) {
        t8 = 255;
        t8++;
        r = t8;
    }

    // Checked prefix dec at 0: uint underflow must revert.
    function underflowDec() external returns (uint256 r) {
        t = 0;
        --t;
        r = t;
    }

    // unchecked postfix on uint8 wraps mod 2^8 like solc.
    function uncheckedWrap() external returns (uint256 r) {
        t8 = 255;
        unchecked { t8++; }
        r = t8;
    }
}
