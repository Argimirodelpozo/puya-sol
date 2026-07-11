// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mGate() { if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; } }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    function dupgate(uint256 a) public mGate() mGate() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }         // SAME modifier twice
    function dparg(uint256 a) public mArg(bump(a)) mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; } // mArg(bump) twice
    function argsuffix(uint256 a) public mGate() mArg(bump(a)) mGate() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; } // exact f2 stack (no super)
}
