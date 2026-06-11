// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    function bump() internal returns (bool) { cnt++; return true; }
    // false && bump() : bump must NOT run -> cnt stays 0, returns false
    function andSC() external returns (bool, uint256) {
        cnt = 0;
        bool r = (1 == 2) && bump();
        return (r, cnt);
    }
    // true || bump() : bump must NOT run -> cnt stays 0, returns true
    function orSC() external returns (bool, uint256) {
        cnt = 0;
        bool r = (1 == 1) || bump();
        return (r, cnt);
    }
    // guard: x != 0 && (100 / x) > 5  with x=0 must NOT divide -> returns false, no panic
    function divGuard(uint256 x) external pure returns (bool) {
        return x != 0 && (100 / x) > 5;
    }
}
