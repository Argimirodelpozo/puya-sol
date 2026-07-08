// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// viaIR regression: calling `super.f()` where the BASE function carries a modifier used to
// fail to compile — buildModifierChain emits the base's `f__mod{i}_N` chain subroutines into
// m_modifierSubroutines, but emitSuperSubroutines never flushed them into the contract → the
// super copy's body referenced an unemitted `f__mod0_N` → puya "unable to resolve function
// reference". Fixed by flushing m_modifierSubroutines after each emitted super target.
// (Found by the generative dispatch fuzzer.) Each modifier/body appends a distinct tag to
// `log` (unchecked) so the value encodes the exact execution path.
contract A {
    uint256 public log;
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    function fBoth() public virtual mBoth() returns (uint256) { unchecked { log = log*100 + 22; } return log; }
    function fPre() public virtual mPre() returns (uint256) { unchecked { log = log*100 + 22; } return log; }
}
contract B is A {
    // override with NO modifier → super target (A.fBoth) has the modifier chain
    function fBoth() public override returns (uint256) { unchecked { log = log*100 + 33; } return super.fBoth(); }
    // override WITH a modifier → both the override and the super target have chains
    function fPre() public override mPre() returns (uint256) { unchecked { log = log*100 + 33; } return super.fPre(); }
}
