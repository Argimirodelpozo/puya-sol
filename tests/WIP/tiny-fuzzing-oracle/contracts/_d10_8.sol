// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED modifier/inheritance/dispatch fixture (fuzz_dispatch.py, tag d10_8).
contract Kd10_8_0 {
    uint256 public log;
    uint256 public sum;   // checked accumulator (exercises multi-run bodies)
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    modifier mPost() { _; unchecked { log = log*100 + 12; } }
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mGate() { if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; } }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function f0(uint256 a) public virtual returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return log; }
    function f1(uint256 a) public virtual mTwice() mPost() mBoth() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return log; }
    function f2(uint256 a) public mPre() { unchecked { log = log*100 + 22 + (a % 7); } sum += (a % 13) + 1; }
    function f3(uint256 a) public virtual mPre() returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return log; }
}
contract Kd10_8_1 is Kd10_8_0 {
    function f0(uint256 a) public override mBoth() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return super.f0(a); }
    function f3(uint256 a) public override virtual mPost() mArg(a % 5) mTwice() returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return super.f3(a); }
}
contract Kd10_8_2 is Kd10_8_1 {
    function f1(uint256 a) public override mBoth() mTwice() mTwice() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return super.f1(a); }
    function f3(uint256 a) public override returns (uint256) { unchecked { log = log*100 + 23 + (a % 7); } return super.f3(a); }
}