// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED modifier/inheritance/dispatch fixture (fuzz_dispatch.py, tag d16_6).
contract Kd16_6_0 {
    uint256 public log;
    uint256 public sum;   // checked accumulator (exercises multi-run bodies)
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    modifier mPost() { _; unchecked { log = log*100 + 12; } }
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mGate() { if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; } }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function f0(uint256 a) public virtual { unchecked { log = log*100 + 20 + (a % 7); } sum += (a % 13) + 1; }
    function f1(uint256 a) public virtual returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return log; }
    function f2(uint256 a) public virtual mPre() mPre() mArg(a % 5) returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
    function f3(uint256 a) public virtual mBoth() mArg(bump(a)) mPre() returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return log; }
}
contract Kd16_6_1 is Kd16_6_0 {
    function f0(uint256 a) public override virtual mBoth() mPre() mPre() { unchecked { log = log*100 + 20 + (a % 7); } sum += (a % 13) + 1; super.f0(a); }
    function f2(uint256 a) public override virtual mBoth() mGate() mTwice() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return super.f2(a); }
    function f3(uint256 a) public override virtual mPost() returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return super.f3(a); }
}
contract Kd16_6_2 is Kd16_6_1 {
    function f0(uint256 a) public override virtual mArg(bump(a)) mPost() { unchecked { log = log*100 + 20 + (a % 7); } sum += (a % 13) + 1; super.f0(a); }
    function f1(uint256 a) public override virtual returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return super.f1(a); }
    function f2(uint256 a) public override virtual returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return super.f2(a); }
    function f3(uint256 a) public override virtual mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return super.f3(a); }
}
contract Kd16_6_3 is Kd16_6_2 {
    function f0(uint256 a) public override { unchecked { log = log*100 + 20 + (a % 7); } sum += (a % 13) + 1; super.f0(a); }
    function f1(uint256 a) public override mPre() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return super.f1(a); }
    function f2(uint256 a) public override returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return super.f2(a); }
    function f3(uint256 a) public override mGate() mBoth() mPre() returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return super.f3(a); }
}