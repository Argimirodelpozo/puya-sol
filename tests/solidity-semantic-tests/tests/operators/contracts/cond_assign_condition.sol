// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    bool flag;
    function f() internal returns (bool) { cnt++; flag = !flag; return flag; }
    // (x = f()) ? 10 : 20  — f() must run exactly ONCE. flag starts false ->
    // f() flips to true, returns true -> result 10, cnt 1.
    function condAssign() external returns (uint256, uint256) {
        cnt = 0; flag = false;
        bool x;
        uint256 r = (x = f()) ? 10 : 20;
        return (r, cnt);   // expect (10, 1)
    }
}
