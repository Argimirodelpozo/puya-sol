// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// NOT an o.g. semantic test. Guards modifier-stacking NESTING ORDER in the body inliner.
// `m() gated both`: Solidity evaluates modifiers left-to-right, so `gated` is OUTERMOST and
// `both`'s ctr++/ctr++ must run INSIDE `gated`'s `if (gate) { _; }`. The pre-fix inliner iterated
// modifiers forward, making the rightmost (`both`) outermost, so ctr incremented even when gate
// was false. With gate=false (default) a call to m() must leave ctr at 0.
contract G {
    uint256 ctr;
    bool gate;
    modifier both() { ctr++; _; ctr++; }
    modifier gated() { if (gate) { _; } }
    function setGate(bool g) external { gate = g; }
    function m() external gated both { }
    function getCtr() external view returns (uint256) { return ctr; }
}
