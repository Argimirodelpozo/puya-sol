// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract AssemblyBalance {
    function self() external view returns (uint256 bal) {
        assembly { bal := selfbalance() }
    }

    function balanceOf(address account) external view returns (uint256 bal) {
        assembly { bal := balance(account) }
    }
}
