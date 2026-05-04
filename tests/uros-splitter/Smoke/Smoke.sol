// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

contract Smoke {
    uint256 public counter;

    function inc(uint256 by) external {
        counter = counter + by;
    }

    function dec(uint256 by) external {
        counter = counter - by;
    }

    function get() external view returns (uint256) {
        return counter;
    }
}
