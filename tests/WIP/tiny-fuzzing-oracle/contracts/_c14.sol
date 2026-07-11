// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract S { uint256 public log;
    function h() internal { unchecked { log = log*100 + 11; } }
    // explicit NESTED blocks (mimics the placeholder wrapper), identical writes, NO modifier
    function f() public { { unchecked { log = log*100 + 33; } h(); } { unchecked { log = log*100 + 33; } h(); } }
}
