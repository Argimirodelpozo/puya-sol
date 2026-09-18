// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract NamedStorageSchema {
    uint256 public a = 41;
    mapping(address => uint256) public bal;
    uint256[] public nums;

    function setBal(address owner, uint256 value) external {
        bal[owner] = value;
    }

    function pushNum(uint256 value) external {
        nums.push(value);
    }
}
