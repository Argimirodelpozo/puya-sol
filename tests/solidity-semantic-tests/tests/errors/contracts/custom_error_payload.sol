// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
error Plain();
error WithArgs(uint256 a, string s);
contract C {
    function p() external pure { revert Plain(); }
    function w() external pure { revert WithArgs(7, "xy"); }
    function r(bool ok) external pure { require(ok, WithArgs(9, "zz")); }
    function rOk() external pure returns (uint256) { require(true, WithArgs(1, "q")); return 5; }
}
