// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED modifier/inheritance/dispatch fixture (fuzz_dispatch.py, tag d3_9).
contract Kd3_9_0 {
    uint256 public log;
    uint256 public sum;   // checked accumulator (exercises multi-run bodies)
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    modifier mPost() { _; unchecked { log = log*100 + 12; } }
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mGate() { if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; } }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function f0(uint256 a) public virtual mGate() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return log; }
    function f1(uint256 a) public virtual { unchecked { log = log*100 + 21 + (a % 7); } sum += (a % 13) + 1; }
    function f2(uint256 a) public virtual mBoth() mPre() { unchecked { log = log*100 + 22 + (a % 7); } sum += (a % 13) + 1; }
    function f3(uint256 a) public virtual mGate() mPre() mTwice() returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return log; }
}
contract Kd3_9_1 is Kd3_9_0 {
    function f1(uint256 a) public override virtual mGate() mArg(bump(a)) { unchecked { log = log*100 + 21 + (a % 7); } sum += (a % 13) + 1; super.f1(a); }
    function f3(uint256 a) public override virtual mTwice() mTwice() returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return super.f3(a); }
}
contract Kd3_9_2 is Kd3_9_1 {
    function f1(uint256 a) public override virtual { unchecked { log = log*100 + 21 + (a % 7); } sum += (a % 13) + 1; super.f1(a); }
    function f2(uint256 a) public override virtual mPost() mTwice() { unchecked { log = log*100 + 22 + (a % 7); } sum += (a % 13) + 1; super.f2(a); }
    function f3(uint256 a) public override virtual mPost() returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return super.f3(a); }
}
contract Kd3_9_3 is Kd3_9_2 {
    function f0(uint256 a) public override mArg(a % 5) mPre() mGate() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return super.f0(a); }
    function f1(uint256 a) public override mBoth() mTwice() { unchecked { log = log*100 + 21 + (a % 7); } sum += (a % 13) + 1; super.f1(a); }
    function f2(uint256 a) public override mPost() { unchecked { log = log*100 + 22 + (a % 7); } sum += (a % 13) + 1; super.f2(a); }
    function f3(uint256 a) public override mPre() mPost() mArg(a % 5) returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return super.f3(a); }
}