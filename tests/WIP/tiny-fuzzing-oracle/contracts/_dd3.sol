// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A { uint256 public log;
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function f(uint256 a) public virtual mArg(bump(a)) mBoth() mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
}
contract B is A {
    function f(uint256 a) public override mTwice() mTwice() returns (uint256) { unchecked { log = log*100 + 33; } return super.f(a); }
}
