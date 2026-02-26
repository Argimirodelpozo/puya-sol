// SPDX-License-Identifier: MIT
pragma solidity >=0.8.4;

error Unauthorized();
error InsufficientBalance(uint256 available, uint256 required);

contract CustomErrorsTest {
    address public owner;
    mapping(address => uint256) public balances;

    constructor() {
        owner = msg.sender;
    }

    function onlyOwnerAction() external view returns (bool) {
        if (msg.sender != owner) {
            revert Unauthorized();
        }
        return true;
    }

    function withdraw(uint256 amount) external view returns (uint256) {
        uint256 bal = balances[msg.sender];
        if (bal < amount) {
            revert InsufficientBalance(bal, amount);
        }
        return bal - amount;
    }

    function deposit(uint256 amount) external {
        balances[msg.sender] = balances[msg.sender] + amount;
    }
}
