// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M43: Multiple contracts in a single file (Gap 4 verification).
 */

contract TokenA {
    mapping(address => uint256) public balances;

    function mint(address to, uint256 amount) external {
        balances[to] += amount;
    }

    function balanceOf(address who) external view returns (uint256) {
        return balances[who];
    }
}

contract TokenB {
    mapping(address => uint256) public balances;
    uint256 public totalMinted;

    function mint(address to, uint256 amount) external {
        balances[to] += amount;
        totalMinted += amount;
    }

    function balanceOf(address who) external view returns (uint256) {
        return balances[who];
    }
}
