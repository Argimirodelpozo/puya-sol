// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A { uint256 public log;
    modifier mPre() { unchecked { log = log*100 + 11; } _; }
    function f() public virtual mPre() returns (uint256) { unchecked { log = log*100 + 22; } return log; }
}
contract B is A { function f() public override mPre() returns (uint256) { unchecked { log = log*100 + 33; } return super.f(); } }
