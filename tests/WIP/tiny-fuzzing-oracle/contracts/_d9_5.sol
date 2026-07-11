// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// GENERATED modifier/inheritance/dispatch fixture (fuzz_dispatch.py, tag d9_5).
contract Kd9_5_0 {
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
    function f1(uint256 a) public virtual mBoth() mPost() mGate() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return log; }
}
contract Kd9_5_1 is Kd9_5_0 {
    function f1(uint256 a) public override virtual mPre() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return super.f1(a); }
}
contract Kd9_5_2 is Kd9_5_1 {
    function f0(uint256 a) public override mPre() mPre() { unchecked { log = log*100 + 20 + (a % 7); } sum += (a % 13) + 1; super.f0(a); }
    function f1(uint256 a) public override mArg(a % 5) mGate() returns (uint256) { unchecked { log = log*100 + 21 + (a % 7); } return super.f1(a); }
}
contract Kd9_5_3 is Kd9_5_2 {
}