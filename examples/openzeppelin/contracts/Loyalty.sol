// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Loyalty {
    address private _admin;
    uint256 private _pointsRate;
    uint256 private _memberCount;
    uint256 private _totalPointsIssued;
    uint256 private _totalPointsRedeemed;

    mapping(address => uint256) internal _points;
    mapping(address => uint256) internal _totalEarned;
    mapping(address => uint256) internal _memberIndex;
    mapping(address => bool) internal _isEnrolled;

    constructor(uint256 pointsRate_) {
        _admin = msg.sender;
        _pointsRate = pointsRate_;
        _memberCount = 0;
        _totalPointsIssued = 0;
        _totalPointsRedeemed = 0;
    }

    function getAdmin() public view returns (address) {
        return _admin;
    }

    function getPointsRate() public view returns (uint256) {
        return _pointsRate;
    }

    function getMemberCount() public view returns (uint256) {
        return _memberCount;
    }

    function getTotalPointsIssued() public view returns (uint256) {
        return _totalPointsIssued;
    }

    function getTotalPointsRedeemed() public view returns (uint256) {
        return _totalPointsRedeemed;
    }

    function getPoints(address member) public view returns (uint256) {
        return _points[member];
    }

    function getTotalEarned(address member) public view returns (uint256) {
        return _totalEarned[member];
    }

    function isEnrolled(address member) public view returns (bool) {
        return _isEnrolled[member];
    }

    function getTier(address member) public view returns (uint256) {
        uint256 earned = _totalEarned[member];
        if (earned >= 5000) {
            return 2;
        } else if (earned >= 1000) {
            return 1;
        } else {
            return 0;
        }
    }

    function enroll(address member) public {
        require(!_isEnrolled[member], "Already enrolled");
        _isEnrolled[member] = true;
        _memberCount = _memberCount + 1;
        _memberIndex[member] = _memberCount;
    }

    function earnPoints(address member, uint256 purchaseAmount) public {
        require(_isEnrolled[member], "Not enrolled");
        uint256 earned = purchaseAmount * _pointsRate;
        _points[member] = _points[member] + earned;
        _totalEarned[member] = _totalEarned[member] + earned;
        _totalPointsIssued = _totalPointsIssued + earned;
    }

    function redeemPoints(address member, uint256 amount) public {
        require(_isEnrolled[member], "Not enrolled");
        require(_points[member] >= amount, "Insufficient points");
        _points[member] = _points[member] - amount;
        _totalPointsRedeemed = _totalPointsRedeemed + amount;
    }

    function setPointsRate(uint256 newRate) public {
        require(msg.sender == _admin, "Not admin");
        _pointsRate = newRate;
    }
}

contract LoyaltyTest is Loyalty {
    constructor() Loyalty(10) {}

    function initMember(address member) external {
        _points[member] = 0;
        _totalEarned[member] = 0;
        _memberIndex[member] = 0;
        _isEnrolled[member] = false;
    }
}
