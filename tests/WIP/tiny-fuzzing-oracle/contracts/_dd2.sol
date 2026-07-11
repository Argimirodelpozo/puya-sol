// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function dupArg(uint256 a) public mArg(bump(a)) mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 22; } return log; }  // bump twice?
    function nestTw(uint256 a) public mTwice() mTwice() returns (uint256) { a; unchecked { log = log*100 + 22; } return log; }         // body 4x?
}
