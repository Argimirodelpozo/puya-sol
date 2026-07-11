// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A { uint256 public log;
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    function fa(uint256 a) public virtual mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 22; } return log; }  // single mArg(bump)
}
contract B is A {
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    // nested double-placeholder over super to a side-effecting-arg base
    function fa(uint256 a) public override mTwice() mTwice() returns (uint256) { unchecked { log = log*100 + 33; } return super.fa(a); }
}
