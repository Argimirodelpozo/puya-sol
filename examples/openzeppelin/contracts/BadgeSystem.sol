// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Achievement badge system.
 * Admin creates badges by hash and can activate/deactivate them.
 * Badges can be awarded, incrementing per-badge and global counters.
 */
abstract contract BadgeSystem {
    address private _admin;
    uint256 private _badgeCount;
    uint256 private _totalAwards;

    mapping(uint256 => uint256) internal _badgeHash;
    mapping(uint256 => uint256) internal _badgeAwards;
    mapping(uint256 => bool) internal _badgeActive;

    constructor() {
        _admin = msg.sender;
        _badgeCount = 0;
        _totalAwards = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getBadgeCount() external view returns (uint256) {
        return _badgeCount;
    }

    function getTotalAwards() external view returns (uint256) {
        return _totalAwards;
    }

    function getBadgeHash(uint256 badgeId) external view returns (uint256) {
        return _badgeHash[badgeId];
    }

    function getAwardCount(uint256 badgeId) external view returns (uint256) {
        return _badgeAwards[badgeId];
    }

    function isBadgeActive(uint256 badgeId) external view returns (bool) {
        return _badgeActive[badgeId];
    }

    function createBadge(uint256 badgeHash) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        uint256 id = _badgeCount;
        _badgeHash[id] = badgeHash;
        _badgeAwards[id] = 0;
        _badgeActive[id] = true;
        _badgeCount = id + 1;
        return id;
    }

    function awardBadge(uint256 badgeId) external {
        require(badgeId < _badgeCount, "Badge does not exist");
        require(_badgeActive[badgeId], "Badge is not active");
        _badgeAwards[badgeId] = _badgeAwards[badgeId] + 1;
        _totalAwards = _totalAwards + 1;
    }

    function deactivateBadge(uint256 badgeId) external {
        require(msg.sender == _admin, "Only admin");
        require(badgeId < _badgeCount, "Badge does not exist");
        require(_badgeActive[badgeId], "Badge already inactive");
        _badgeActive[badgeId] = false;
    }

    function activateBadge(uint256 badgeId) external {
        require(msg.sender == _admin, "Only admin");
        require(badgeId < _badgeCount, "Badge does not exist");
        require(!_badgeActive[badgeId], "Badge already active");
        _badgeActive[badgeId] = true;
    }
}

contract BadgeSystemTest is BadgeSystem {
    constructor() BadgeSystem() {}

    function initBadge(uint256 badgeId) external {
        _badgeHash[badgeId] = 0;
        _badgeAwards[badgeId] = 0;
        _badgeActive[badgeId] = false;
    }
}
