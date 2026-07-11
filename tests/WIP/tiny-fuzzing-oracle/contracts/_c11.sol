// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function h() internal { unchecked { log = log*100 + 11; } }
    function f(uint256 a) public mTwice() { a; unchecked { log = log*100 + 33; } h(); }  // h() as statement, void body
}
