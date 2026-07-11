// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function f(uint256 a) public mTwice() mBoth() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
    function g(uint256 a) public mBoth() mTwice() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
}
