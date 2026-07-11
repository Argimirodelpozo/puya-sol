// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    function h() internal { unchecked { log = log*100 + 11; } }
    function f() public { h(); h(); }          // two identical calls, NO modifier
    function g() public { unchecked { log = log*100 + 33; } h(); unchecked { log = log*100 + 44; } h(); }  // calls separated by state writes
}
