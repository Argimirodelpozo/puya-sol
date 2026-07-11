// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A { uint256 public log;
    function bump(uint256 v) internal returns (uint256) { unchecked { log = log*100 + 90 + (v % 9); } return v; }
    modifier mArg(uint256 v) { unchecked { log = log*100 + 70 + (v % 9); } _; }
    modifier mTwice() { unchecked { log = log*100 + 15; } _; _; }
    function fa(uint256 a) public virtual mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 22; } return log; }               // single mArg(bump)
    function fb(uint256 a) public virtual mArg(bump(a)) mArg(bump(a)) returns (uint256) { unchecked { log = log*100 + 22; } return log; }  // dup mArg(bump)
}
contract B is A {
    // (1) single mTwice over super -> single mArg(bump) base
    function fa(uint256 a) public override mTwice() returns (uint256) { unchecked { log = log*100 + 33; } return super.fa(a); }
    // (2) NO override modifier + super -> dup mArg(bump) base
    function fb(uint256 a) public override returns (uint256) { unchecked { log = log*100 + 33; } return super.fb(a); }
}
