// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED modifier/inheritance/dispatch fixture (fuzz_dispatch.py, tag d14_6).
contract Kd14_6_0 {
    uint256 public log;
    uint256 public sum;   // checked accumulator (exercises multi-run bodies)
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    modifier mPost() { _; unchecked { log = log*100 + 12; } }
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mGate() { if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; } }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function f0(uint256 a) public virtual mGate() mTwice() mGate() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return log; }
    function f1(uint256 a) public virtual returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return log; }
}
contract Kd14_6_1 is Kd14_6_0 {
    function f0(uint256 a) public override virtual mArg(bump(a)) mBoth() mTwice() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return super.f0(a); }
    function f1(uint256 a) public override virtual mPost() mBoth() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return super.f1(a); }
}
contract Kd14_6_2 is Kd14_6_1 {
    function f0(uint256 a) public override virtual mPost() mPre() mGate() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return log; }
    function f1(uint256 a) public override mGate() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return super.f1(a); }
}
contract Kd14_6_3 is Kd14_6_2 {
    function f0(uint256 a) public override mGate() mBoth() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return log; }
}