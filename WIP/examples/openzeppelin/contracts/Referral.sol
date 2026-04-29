// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Referral {
    address private admin;
    uint256 private rewardRate;
    uint256 private totalReferrals;
    uint256 private totalRewards;
    uint256 private memberCount;

    mapping(address => address) internal _referrer;
    mapping(address => uint256) internal _referralCount;
    mapping(address => uint256) internal _rewards;
    mapping(address => uint256) internal _memberIndex;

    constructor(uint256 rewardRate_) {
        admin = msg.sender;
        rewardRate = rewardRate_;
    }

    function register(address member, address referrer) public {
        _referrer[member] = referrer;
        if (_memberIndex[member] == 0) {
            memberCount = memberCount + 1;
            _memberIndex[member] = memberCount;
        }
    }

    function recordReferral(address referrer, uint256 purchaseAmount) public {
        uint256 reward = purchaseAmount * rewardRate / 10000;
        _rewards[referrer] = _rewards[referrer] + reward;
        _referralCount[referrer] = _referralCount[referrer] + 1;
        totalReferrals = totalReferrals + 1;
        totalRewards = totalRewards + reward;
    }

    function claimReward(address member) public returns (uint256) {
        require(_rewards[member] > 0);
        uint256 amount = _rewards[member];
        _rewards[member] = 0;
        return amount;
    }

    function getReferrer(address member) public view returns (address) {
        return _referrer[member];
    }

    function getReferralCount(address member) public view returns (uint256) {
        return _referralCount[member];
    }

    function getRewards(address member) public view returns (uint256) {
        return _rewards[member];
    }

    function getTotalReferrals() public view returns (uint256) {
        return totalReferrals;
    }

    function getTotalRewards() public view returns (uint256) {
        return totalRewards;
    }

    function getMemberCount() public view returns (uint256) {
        return memberCount;
    }

    function getRewardRate() public view returns (uint256) {
        return rewardRate;
    }

    function setRewardRate(uint256 newRate) public {
        require(msg.sender == admin);
        rewardRate = newRate;
    }

    function getAdmin() public view returns (address) {
        return admin;
    }
}

contract ReferralTest is Referral {
    constructor() Referral(200) {}

    function initMember(address member) public {
        _referrer[member] = address(0);
        _referralCount[member] = 0;
        _rewards[member] = 0;
        _memberIndex[member] = 0;
    }
}
