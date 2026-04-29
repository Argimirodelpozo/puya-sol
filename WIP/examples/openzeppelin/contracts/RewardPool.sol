// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract RewardPool {
    address private _admin;
    uint256 private _recipientCount;
    uint256 private _totalDistributed;
    uint256 private _totalClaimed;

    mapping(uint256 => uint256) internal _recipientHash;
    mapping(uint256 => uint256) internal _pendingReward;
    mapping(uint256 => uint256) internal _claimedReward;

    constructor() {
        _admin = msg.sender;
        _recipientCount = 0;
        _totalDistributed = 0;
        _totalClaimed = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getRecipientCount() external view returns (uint256) {
        return _recipientCount;
    }

    function getTotalDistributed() external view returns (uint256) {
        return _totalDistributed;
    }

    function getTotalClaimed() external view returns (uint256) {
        return _totalClaimed;
    }

    function getRecipientHash(uint256 recipientId) external view returns (uint256) {
        return _recipientHash[recipientId];
    }

    function getPendingReward(uint256 recipientId) external view returns (uint256) {
        return _pendingReward[recipientId];
    }

    function getClaimedReward(uint256 recipientId) external view returns (uint256) {
        return _claimedReward[recipientId];
    }

    function addRecipient(uint256 recipientHash) external returns (uint256) {
        uint256 id = _recipientCount;
        _recipientHash[id] = recipientHash;
        _pendingReward[id] = 0;
        _claimedReward[id] = 0;
        _recipientCount = id + 1;
        return id;
    }

    function distributeReward(uint256 recipientId, uint256 amount) external {
        require(msg.sender == _admin, "Not admin");
        _pendingReward[recipientId] = _pendingReward[recipientId] + amount;
        _totalDistributed = _totalDistributed + amount;
    }

    function claimReward(uint256 recipientId) external {
        uint256 pending = _pendingReward[recipientId];
        require(pending > 0, "No pending reward");
        _pendingReward[recipientId] = 0;
        _claimedReward[recipientId] = _claimedReward[recipientId] + pending;
        _totalClaimed = _totalClaimed + pending;
    }
}

contract RewardPoolTest is RewardPool {
    constructor() RewardPool() {}

    function initRecipient(uint256 recipientId) external {
        _recipientHash[recipientId] = 0;
        _pendingReward[recipientId] = 0;
        _claimedReward[recipientId] = 0;
    }
}
