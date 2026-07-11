// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    uint256 public log;
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mGate() { if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; } }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    function g_gate(uint256 a) public mGate() returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return log; }
    function g_arg(uint256 a) public mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return log; }
    function g_both(uint256 a) public mGate() mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 20 + (a % 7); } return log; }
}
