// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function argtwice(uint256 a) public mArg(bump(a)) mBoth() mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }  // base f2 shape
    function twotw(uint256 a) public mTwice() mTwice() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }  // nested double-placeholder (4x body)
}
