// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// CUSTOM puya-sol test contract (NOT vendored from the upstream Solidity
// semantic suite) — guards a compiler fix. A tuple-destructured local that
// shadows an outer variable must get a shadow-safe unique name (like the
// single-declaration path), else the inner destructured var aliases and
// overwrites the outer one. Before the fix, shadowTuple() returned 1 (the inner
// `a` from two()) instead of 100 (the outer `a`).
contract C {
    function two() internal pure returns (uint, uint) { return (1, 2); }
    function shadowTuple() external pure returns (uint) {
        uint a = 100;
        { (uint a, uint b) = two(); a; b; }
        return a;
    }
    // single-decl shadowing was already correct — guard it stays so
    function shadowSingle() external pure returns (uint) {
        uint a = 100;
        { uint a = 5; a; }
        return a;
    }
}
