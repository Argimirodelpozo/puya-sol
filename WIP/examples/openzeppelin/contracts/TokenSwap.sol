// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simple token swap with fixed exchange rate.
 * Simulates swapping between two token balances at a configurable rate.
 */
contract TokenSwapTest {
    address private _owner;
    uint256 private _rate; // tokenB per tokenA (scaled by 1000)
    uint256 private _totalSwapped;

    mapping(address => uint256) private _balanceA;
    mapping(address => uint256) private _balanceB;

    constructor() {
        _owner = msg.sender;
        _rate = 2000; // 2.0 tokenB per tokenA (2000/1000)
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function rate() external view returns (uint256) {
        return _rate;
    }

    function totalSwapped() external view returns (uint256) {
        return _totalSwapped;
    }

    function setRate(uint256 newRate) external {
        require(msg.sender == _owner, "Not owner");
        require(newRate > 0, "Rate must be > 0");
        _rate = newRate;
    }

    function balanceA(address account) external view returns (uint256) {
        return _balanceA[account];
    }

    function balanceB(address account) external view returns (uint256) {
        return _balanceB[account];
    }

    function depositA(address account, uint256 amount) external {
        _balanceA[account] += amount;
    }

    function depositB(address account, uint256 amount) external {
        _balanceB[account] += amount;
    }

    function swapAtoB(address account, uint256 amountA) external returns (uint256) {
        require(_balanceA[account] >= amountA, "Insufficient balance A");

        uint256 amountB = (amountA * _rate) / 1000;
        _balanceA[account] -= amountA;
        _balanceB[account] += amountB;
        _totalSwapped += amountA;

        return amountB;
    }

    function swapBtoA(address account, uint256 amountB) external returns (uint256) {
        require(_balanceB[account] >= amountB, "Insufficient balance B");

        uint256 amountA = (amountB * 1000) / _rate;
        _balanceB[account] -= amountB;
        _balanceA[account] += amountA;
        _totalSwapped += amountA;

        return amountA;
    }

    function getQuoteAtoB(uint256 amountA) external view returns (uint256) {
        return (amountA * _rate) / 1000;
    }

    function getQuoteBtoA(uint256 amountB) external view returns (uint256) {
        return (amountB * 1000) / _rate;
    }
}
