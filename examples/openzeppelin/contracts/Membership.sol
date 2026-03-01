// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Membership {
    address private _admin;
    uint256 private _memberCount;
    uint256 private _totalFees;
    uint256 private _basicFee;
    uint256 private _premiumFee;
    uint256 private _vipFee;

    mapping(address => uint256) internal _memberTier;
    mapping(address => uint256) internal _memberFeesPaid;
    mapping(address => bool) internal _isMember;
    mapping(address => bool) internal _isSuspended;
    mapping(address => uint256) internal _memberIndex;

    constructor(uint256 basicFee_, uint256 premiumFee_, uint256 vipFee_) {
        _admin = msg.sender;
        _basicFee = basicFee_;
        _premiumFee = premiumFee_;
        _vipFee = vipFee_;
        _memberCount = 0;
        _totalFees = 0;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function memberCount() external view returns (uint256) {
        return _memberCount;
    }

    function totalFees() external view returns (uint256) {
        return _totalFees;
    }

    function basicFee() external view returns (uint256) {
        return _basicFee;
    }

    function premiumFee() external view returns (uint256) {
        return _premiumFee;
    }

    function vipFee() external view returns (uint256) {
        return _vipFee;
    }

    function addMember(address member, uint256 tier) external {
        require(!_isMember[member], "Already a member");
        require(tier <= 2, "Invalid tier");
        _isMember[member] = true;
        _memberTier[member] = tier;
        _memberFeesPaid[member] = 0;
        _isSuspended[member] = false;
        _memberCount = _memberCount + 1;
        _memberIndex[member] = _memberCount;
    }

    function payFee(address member, uint256 amount) external {
        require(_isMember[member], "Not a member");
        require(!_isSuspended[member], "Member is suspended");
        _memberFeesPaid[member] = _memberFeesPaid[member] + amount;
        _totalFees = _totalFees + amount;
    }

    function getFeeForTier(uint256 tier) external view returns (uint256) {
        require(tier <= 2, "Invalid tier");
        if (tier == 0) {
            return _basicFee;
        } else if (tier == 1) {
            return _premiumFee;
        }
        return _vipFee;
    }

    function suspend(address member) external {
        require(_isMember[member], "Not a member");
        _isSuspended[member] = true;
    }

    function reinstate(address member) external {
        require(_isMember[member], "Not a member");
        _isSuspended[member] = false;
    }

    function upgradeTier(address member, uint256 newTier) external {
        require(_isMember[member], "Not a member");
        require(newTier > _memberTier[member], "New tier must be higher");
        require(newTier <= 2, "Invalid tier");
        _memberTier[member] = newTier;
    }

    function getMemberTier(address member) external view returns (uint256) {
        return _memberTier[member];
    }

    function getMemberFeesPaid(address member) external view returns (uint256) {
        return _memberFeesPaid[member];
    }

    function isMember(address member) external view returns (bool) {
        return _isMember[member];
    }

    function isSuspended(address member) external view returns (bool) {
        return _isSuspended[member];
    }
}

contract MembershipTest is Membership {
    constructor() Membership(100, 500, 1000) {}

    function initMember(address member) external {
        _memberTier[member] = 0;
        _memberFeesPaid[member] = 0;
        _isMember[member] = false;
        _isSuspended[member] = false;
        _memberIndex[member] = 0;
    }
}
