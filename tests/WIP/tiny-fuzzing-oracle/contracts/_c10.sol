// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function h() internal returns (uint256) { unchecked { log = log*100 + 11; } unchecked { log = log*100 + 22; } return log; }
    function f(uint256 a) public mTwice() returns (uint256) { a; unchecked { log = log*100 + 33; } return h(); }
}
