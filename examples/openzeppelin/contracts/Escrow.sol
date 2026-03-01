// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simplified Escrow pattern from OpenZeppelin.
 * Tracks deposits per address and allows withdrawal by owner.
 */
contract EscrowTest {
    address private _owner;
    mapping(address => uint256) private _deposits;
    uint256 private _totalDeposits;

    constructor() {
        _owner = msg.sender;
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function depositsOf(address payee) external view returns (uint256) {
        return _deposits[payee];
    }

    function totalDeposits() external view returns (uint256) {
        return _totalDeposits;
    }

    function deposit(address payee, uint256 amount) external {
        _deposits[payee] += amount;
        _totalDeposits += amount;
    }

    function withdraw(address payee, uint256 amount) external {
        require(msg.sender == _owner, "Escrow: caller is not the owner");
        require(_deposits[payee] >= amount, "Escrow: insufficient deposit");
        _deposits[payee] -= amount;
        _totalDeposits -= amount;
    }
}
