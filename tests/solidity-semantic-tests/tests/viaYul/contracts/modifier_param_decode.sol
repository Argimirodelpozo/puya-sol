// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// viaIR regression: a modifier'd function whose body (or a modifier's arg) does biguint
// arithmetic on a uint256 PARAM used to REVERT ("b% wanted bigint but got uint64").
// buildModifierChain moves the body into a `__body_N` sub and threads the still-ARC4-
// encoded params through the chain, but the ARC4 param decode was inserted only into the
// OUTER method — so the sub used an undecoded param. Fixed by prepending a clone of the
// decodes into every emitted chain sub. Each tag appended to `log` (unchecked) so the
// value encodes the path. (Found by the generative dispatch fuzzer.)
contract P {
    uint256 public log;
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }               // multiple placeholder
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; } // modifier ARG uses a param

    // body uses the param under a single-placeholder modifier
    function body1(uint256 a) public mPre() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
    // body uses the param under a MULTIPLE-placeholder modifier (body + param-read run twice)
    function body2(uint256 a) public mTwice() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
    // a MODIFIER ARG uses the param (evaluated in the modifier sub, not the body)
    function arg1(uint256 a) public mArg(a % 5) returns (uint256) { unchecked { log = log*100 + 22; } return log; }
}
