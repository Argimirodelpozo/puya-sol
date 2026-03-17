// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Mapping operations regression test.
contract Mappings {
    mapping(address => uint256) public balances;
    mapping(address => mapping(address => uint256)) public allowances;
    mapping(uint256 => bool) public flags;

    function setBalance(address who, uint256 amount) external {
        balances[who] = amount;
    }

    function getBalance(address who) external view returns (uint256) {
        return balances[who];
    }

    function setAllowance(address owner, address spender, uint256 amount) external {
        allowances[owner][spender] = amount;
    }

    function getAllowance(address owner, address spender) external view returns (uint256) {
        return allowances[owner][spender];
    }

    function setFlag(uint256 key, bool val) external {
        flags[key] = val;
    }

    function getFlag(uint256 key) external view returns (bool) {
        return flags[key];
    }

    function incrementBalance(address who, uint256 amount) external {
        balances[who] += amount;
    }

    function decrementBalance(address who, uint256 amount) external {
        balances[who] -= amount;
    }
}
