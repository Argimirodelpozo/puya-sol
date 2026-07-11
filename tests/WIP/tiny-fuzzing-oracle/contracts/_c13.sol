// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    function h() internal { unchecked { log = log*100 + 11; } }
    // NO modifier: two IDENTICAL `log*100+33` writes separated by a state-mutating call
    function f() public { unchecked { log = log*100 + 33; } h(); unchecked { log = log*100 + 33; } h(); }
}
