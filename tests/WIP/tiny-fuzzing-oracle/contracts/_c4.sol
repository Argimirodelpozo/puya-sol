// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract A { uint256 public log;
    modifier mBoth() { unchecked { log = log*100 + 13; } _; unchecked { log = log*100 + 14; } }
    modifier mGate() { if (log % 2 == 0) { unchecked { log = log*100 + 16; } _; } else { _; } }
    function f() public virtual mGate() mBoth() returns (uint256) { unchecked { log = log*100 + 22; } return log; }
}
contract B is A { function f() public override returns (uint256) { unchecked { log = log*100 + 33; } return super.f(); } }
