// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    function h() internal { unchecked { log = log*100 + 11; } }
    function f(uint256 a) public { a; unchecked { log = log*100 + 15; } { unchecked { log = log*100 + 33; } h(); } { unchecked { log = log*100 + 33; } h(); } }
}
