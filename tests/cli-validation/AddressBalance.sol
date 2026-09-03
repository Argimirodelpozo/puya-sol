// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract AddressBalance {
    function read(address account) external view returns (uint256) {
        return account.balance;
    }
}
