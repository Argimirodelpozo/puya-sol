// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    function tb(uint256 a) public mTwice() mBoth() returns (uint256) { unchecked { log = log*100 + 22; } return log; } // mTwice outer, mBoth inner
    function bt(uint256 a) public mBoth() mTwice() returns (uint256) { unchecked { log = log*100 + 22; } return log; } // mBoth outer, mTwice inner
    function tp(uint256 a) public mTwice() mPre() returns (uint256) { unchecked { log = log*100 + 22; } return log; }  // mTwice outer, mPre inner (single)
    function pt(uint256 a) public mPre() mTwice() returns (uint256) { unchecked { log = log*100 + 22; } return log; }  // mPre outer, mTwice inner
}
