// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Fee collector with configurable fee tiers and withdrawal.
 */
contract FeeCollectorTest {
    address private _owner;
    uint256 private _totalCollected;
    uint256 private _totalWithdrawn;
    uint256 private _defaultFeeRate; // basis points (100 = 1%)

    mapping(address => uint256) private _customFeeRate;
    mapping(address => uint256) private _feesPaid;

    constructor() {
        _owner = msg.sender;
        _defaultFeeRate = 300; // 3%
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function totalCollected() external view returns (uint256) {
        return _totalCollected;
    }

    function totalWithdrawn() external view returns (uint256) {
        return _totalWithdrawn;
    }

    function defaultFeeRate() external view returns (uint256) {
        return _defaultFeeRate;
    }

    function setDefaultFeeRate(uint256 rate) external {
        require(msg.sender == _owner, "Not owner");
        require(rate <= 5000, "Max 50%");
        _defaultFeeRate = rate;
    }

    function setCustomFeeRate(address account, uint256 rate) external {
        require(msg.sender == _owner, "Not owner");
        _customFeeRate[account] = rate;
    }

    function getFeeRate(address account) external view returns (uint256) {
        uint256 custom = _customFeeRate[account];
        if (custom > 0) return custom;
        return _defaultFeeRate;
    }

    function calculateFee(address account, uint256 amount) external view returns (uint256) {
        uint256 custom = _customFeeRate[account];
        uint256 rate = custom > 0 ? custom : _defaultFeeRate;
        return (amount * rate) / 10000;
    }

    function collectFee(address account, uint256 amount) external returns (uint256) {
        uint256 custom = _customFeeRate[account];
        uint256 rate = custom > 0 ? custom : _defaultFeeRate;
        uint256 fee = (amount * rate) / 10000;

        _feesPaid[account] += fee;
        _totalCollected += fee;

        return fee;
    }

    function feesPaid(address account) external view returns (uint256) {
        return _feesPaid[account];
    }

    function withdraw(uint256 amount) external {
        require(msg.sender == _owner, "Not owner");
        require(amount <= _totalCollected - _totalWithdrawn, "Insufficient balance");
        _totalWithdrawn += amount;
    }

    function availableBalance() external view returns (uint256) {
        return _totalCollected - _totalWithdrawn;
    }
}
