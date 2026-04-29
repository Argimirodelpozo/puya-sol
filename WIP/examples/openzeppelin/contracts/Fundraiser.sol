// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Fundraiser {
    address private _admin;
    uint256 private _campaignCount;
    uint256 private _totalRaised;

    mapping(uint256 => uint256) internal _campaignGoal;
    mapping(uint256 => uint256) internal _campaignRaised;
    mapping(uint256 => uint256) internal _campaignDeadline;
    mapping(uint256 => bool) internal _campaignFinalized;
    mapping(uint256 => bool) internal _campaignRefunded;
    mapping(uint256 => address) internal _campaignCreator;

    constructor() {
        _admin = msg.sender;
        _campaignCount = 0;
        _totalRaised = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getCampaignCount() external view returns (uint256) {
        return _campaignCount;
    }

    function getTotalRaised() external view returns (uint256) {
        return _totalRaised;
    }

    function getCampaignGoal(uint256 id) external view returns (uint256) {
        return _campaignGoal[id];
    }

    function getCampaignRaised(uint256 id) external view returns (uint256) {
        return _campaignRaised[id];
    }

    function getCampaignDeadline(uint256 id) external view returns (uint256) {
        return _campaignDeadline[id];
    }

    function isCampaignFinalized(uint256 id) external view returns (bool) {
        return _campaignFinalized[id];
    }

    function isCampaignRefunded(uint256 id) external view returns (bool) {
        return _campaignRefunded[id];
    }

    function getCampaignCreator(uint256 id) external view returns (address) {
        return _campaignCreator[id];
    }

    function isGoalReached(uint256 id) external view returns (bool) {
        return _campaignRaised[id] >= _campaignGoal[id];
    }

    function isExpired(uint256 id, uint256 currentTime) external view returns (bool) {
        return currentTime > _campaignDeadline[id];
    }

    function createCampaign(address creator, uint256 goal, uint256 deadline) external returns (uint256) {
        uint256 campaignId = _campaignCount;
        _campaignGoal[campaignId] = goal;
        _campaignRaised[campaignId] = 0;
        _campaignDeadline[campaignId] = deadline;
        _campaignFinalized[campaignId] = false;
        _campaignRefunded[campaignId] = false;
        _campaignCreator[campaignId] = creator;
        _campaignCount = campaignId + 1;
        return campaignId;
    }

    function contribute(uint256 campaignId, uint256 amount) external {
        require(!_campaignFinalized[campaignId], "Campaign already finalized");
        require(!_campaignRefunded[campaignId], "Campaign already refunded");
        _campaignRaised[campaignId] = _campaignRaised[campaignId] + amount;
        _totalRaised = _totalRaised + amount;
    }

    function finalizeCampaign(uint256 campaignId) external {
        require(!_campaignFinalized[campaignId], "Campaign already finalized");
        _campaignFinalized[campaignId] = true;
    }

    function refundCampaign(uint256 campaignId) external {
        require(!_campaignFinalized[campaignId], "Campaign already finalized");
        require(!_campaignRefunded[campaignId], "Campaign already refunded");
        _campaignRefunded[campaignId] = true;
        _totalRaised = _totalRaised - _campaignRaised[campaignId];
    }
}

contract FundraiserTest is Fundraiser {
    constructor() Fundraiser() {}

    function initCampaign(uint256 campaignId) external {
        _campaignGoal[campaignId] = 0;
        _campaignRaised[campaignId] = 0;
        _campaignDeadline[campaignId] = 0;
        _campaignFinalized[campaignId] = false;
        _campaignRefunded[campaignId] = false;
        _campaignCreator[campaignId] = address(0);
    }
}
