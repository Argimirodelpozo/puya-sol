// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A {
    uint256 public log;
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mGate() { if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; } }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function f(uint256 a) public virtual mGate() mBoth() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
    function fg(uint256 a) public virtual mGate() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
    function fb(uint256 a) public virtual mBoth() returns (uint256) { unchecked { log = log*100 + 22 + (a % 7); } return log; }
}
contract B is A {
    // super chain: derived mTwice wrapper -> base mGate mBoth
    function f(uint256 a) public override mTwice() returns (uint256) { unchecked { log = log*100 + 33 + (a % 7); } return super.f(a); }
    function fg(uint256 a) public override returns (uint256) { unchecked { log = log*100 + 33 + (a % 7); } return super.fg(a); }  // super to gated base, no deriv modifier
    function fb(uint256 a) public override returns (uint256) { unchecked { log = log*100 + 33 + (a % 7); } return super.fb(a); }  // super to mBoth base
}
