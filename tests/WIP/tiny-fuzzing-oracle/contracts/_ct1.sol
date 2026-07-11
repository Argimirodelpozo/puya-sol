// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function fret(uint256 a) public mTwice() returns (uint256) { unchecked { log = log*100 + 22; } return log; }   // mTwice + RETURN
    function fvoid(uint256 a) public mTwice() { a; unchecked { log = log*100 + 22; } }                             // mTwice + void
}
