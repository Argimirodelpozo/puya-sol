// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A { uint256 public log;
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    function fArg(uint256 a) public virtual mArg(a % 5) returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }  // modifier ARG uses param
}
contract B is A {
    function fArg(uint256 a) public override mArg(a % 5) returns (uint256) { unchecked { log = log*100 + 33; } return super.fArg(a); } // override mArg(param) + super
}
