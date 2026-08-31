// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Modifier-semantics probe cells (solc appendModifierOrFunctionCode).
contract ModProbe {
    uint256 public trace;

    function reset() public { trace = 0; }

    function _mark(uint256 d) internal { trace = trace * 10 + d; }

    // M1: body `return` — the post-_; epilogue must still run.
    modifier epi() {
        _mark(1);
        _;
        _mark(2);
    }
    function m1() public epi returns (uint256) {
        trace = 0;
        _mark(7);
        return 99;
    }

    // M2: modifier returns BEFORE _; — body skipped, outer epilogue runs.
    modifier gate(bool go) {
        _mark(3);
        if (!go) return;
        _;
        _mark(4);
    }
    function m2() public epi gate(false) returns (uint256 r) {
        _mark(8);   // must NOT run
        r = 55;     // named return keeps its DEFAULT (0) when body is skipped
    }

    // M3: multiple _; — body runs per placeholder, even after a body return.
    modifier twice() {
        _;
        _mark(5);
        _;
        _mark(6);
    }
    function m3() public twice returns (uint256) {
        _mark(9);
        return 0;
    }

    // M6: epilogue runs after an explicit body return (state-observable).
    uint256 public stash;
    modifier clamp() {
        stash = 0;
        _;
        stash = 111;
    }
    function m6(uint256 v) public clamp returns (uint256 r) {
        r = v + 1;
        return r;
    }

    // M4: each modifier's ARG evaluates on scope entry, seeing earlier
    // modifiers' writes.
    modifier logArg(uint256 v) {
        _mark(v);
        _;
    }
    function m4() public logArg(1) logArg(trace + 1) returns (uint256) {
        return trace;
    }

    // M5: constructor modifier wraps the ctor body.
    uint256 public ctorTrace;
    modifier cmod() {
        ctorTrace = ctorTrace * 10 + 3;
        _;
        ctorTrace = ctorTrace * 10 + 4;
    }
    constructor() cmod {
        ctorTrace = ctorTrace * 10 + 7;
    }

    // M8: same modifier stacked twice.
    function m8() public epi epi returns (uint256) {
        trace = 0;
        _mark(9);
        return 0;
    }
}
