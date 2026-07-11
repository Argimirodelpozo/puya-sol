// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function single(uint256 a) public mPre() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }  // single _;, param in body
    function dbl(uint256 a) public mTwice() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }    // double _;, param in body
}
