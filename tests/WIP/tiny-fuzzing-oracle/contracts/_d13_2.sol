// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED modifier/inheritance/dispatch fixture (fuzz_dispatch.py, tag d13_2).
contract Kd13_2_0 {
    uint256 public log;
    uint256 public sum;   // checked accumulator (exercises multi-run bodies)
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    modifier mPost() { _; unchecked { log = log*100 + 12; } }
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mGate() { if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; } }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function f0(uint256 a) public virtual mArg(bump(a)) mArg(a % 5) mTwice() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return log; }
    function f1(uint256 a) public virtual mBoth() mPre() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return log; }
    function f2(uint256 a) public virtual mBoth() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
    function f3(uint256 a) public mArg(a % 5) mPost() returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return log; }
}
contract Kd13_2_1 is Kd13_2_0 {
    function f0(uint256 a) public override virtual mPost() mBoth() mBoth() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return super.f0(a); }
}
contract Kd13_2_2 is Kd13_2_1 {
    function f1(uint256 a) public override virtual mTwice() mPre() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return log; }
    function f2(uint256 a) public override virtual mArg(bump(a)) mTwice() mGate() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
}
contract Kd13_2_3 is Kd13_2_2 {
    function f0(uint256 a) public override mBoth() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return super.f0(a); }
    function f1(uint256 a) public override mTwice() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return super.f1(a); }
    function f2(uint256 a) public override mGate() mArg(bump(a)) mGate() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return super.f2(a); }
}