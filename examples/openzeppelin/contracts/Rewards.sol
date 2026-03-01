// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Points-based rewards system.
 * Users earn points for actions, can redeem for rewards, tracks levels.
 */
contract RewardsTest {
    address private _owner;
    uint256 private _pointsPerAction;
    uint256 private _redemptionRate; // points per unit redeemed
    uint256 private _totalPointsIssued;
    uint256 private _totalRedeemed;

    mapping(address => uint256) private _points;
    mapping(address => uint256) private _totalEarned;
    mapping(address => uint256) private _totalSpent;

    constructor() {
        _owner = msg.sender;
        _pointsPerAction = 10;
        _redemptionRate = 100; // 100 points = 1 unit
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function pointsPerAction() external view returns (uint256) {
        return _pointsPerAction;
    }

    function redemptionRate() external view returns (uint256) {
        return _redemptionRate;
    }

    function totalPointsIssued() external view returns (uint256) {
        return _totalPointsIssued;
    }

    function totalRedeemed() external view returns (uint256) {
        return _totalRedeemed;
    }

    function pointsOf(address account) external view returns (uint256) {
        return _points[account];
    }

    function totalEarnedOf(address account) external view returns (uint256) {
        return _totalEarned[account];
    }

    function totalSpentOf(address account) external view returns (uint256) {
        return _totalSpent[account];
    }

    function setPointsPerAction(uint256 amount) external {
        require(msg.sender == _owner, "Not owner");
        _pointsPerAction = amount;
    }

    function earnPoints(address account, uint256 actions) external {
        uint256 earned = actions * _pointsPerAction;
        _points[account] += earned;
        _totalEarned[account] += earned;
        _totalPointsIssued += earned;
    }

    function awardBonus(address account, uint256 bonusPoints) external {
        require(msg.sender == _owner, "Not owner");
        _points[account] += bonusPoints;
        _totalEarned[account] += bonusPoints;
        _totalPointsIssued += bonusPoints;
    }

    function redeem(address account, uint256 units) external returns (uint256) {
        uint256 cost = units * _redemptionRate;
        require(_points[account] >= cost, "Insufficient points");

        _points[account] -= cost;
        _totalSpent[account] += cost;
        _totalRedeemed += units;

        return cost;
    }

    function getLevel(address account) external view returns (uint256) {
        uint256 total = _totalEarned[account];
        if (total >= 10000) return 5;
        if (total >= 5000) return 4;
        if (total >= 1000) return 3;
        if (total >= 100) return 2;
        if (total > 0) return 1;
        return 0;
    }

    function redeemableUnits(address account) external view returns (uint256) {
        return _points[account] / _redemptionRate;
    }
}
