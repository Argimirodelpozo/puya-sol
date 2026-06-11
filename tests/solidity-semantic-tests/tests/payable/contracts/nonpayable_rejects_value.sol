// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public x;
    function plain(uint256 v) external { x = v; }            // non-payable
    function paid(uint256 v) external payable { x = v; }     // payable
}
