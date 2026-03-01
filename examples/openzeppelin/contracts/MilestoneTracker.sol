// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Project milestone tracking with completion status.
 * Admin creates milestones with a target value, updates progress,
 * and marks them complete. Tracks total milestones and completed count.
 */
abstract contract MilestoneTracker {
    address private _admin;
    uint256 private _milestoneCount;
    uint256 private _completedCount;

    mapping(uint256 => uint256) internal _milestoneTarget;
    mapping(uint256 => uint256) internal _milestoneProgress;
    mapping(uint256 => bool) internal _milestoneCompleted;

    constructor() {
        _admin = msg.sender;
        _milestoneCount = 0;
        _completedCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getMilestoneCount() external view returns (uint256) {
        return _milestoneCount;
    }

    function getCompletedCount() external view returns (uint256) {
        return _completedCount;
    }

    function getMilestoneTarget(uint256 milestoneId) external view returns (uint256) {
        return _milestoneTarget[milestoneId];
    }

    function getProgress(uint256 milestoneId) external view returns (uint256) {
        return _milestoneProgress[milestoneId];
    }

    function isCompleted(uint256 milestoneId) external view returns (bool) {
        return _milestoneCompleted[milestoneId];
    }

    function createMilestone(uint256 target) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        require(target > 0, "Target must be positive");
        uint256 id = _milestoneCount;
        _milestoneTarget[id] = target;
        _milestoneProgress[id] = 0;
        _milestoneCompleted[id] = false;
        _milestoneCount = id + 1;
        return id;
    }

    function updateProgress(uint256 milestoneId, uint256 amount) external {
        require(msg.sender == _admin, "Only admin");
        require(milestoneId < _milestoneCount, "Milestone does not exist");
        require(!_milestoneCompleted[milestoneId], "Milestone already completed");
        require(amount > 0, "Amount must be positive");
        _milestoneProgress[milestoneId] = _milestoneProgress[milestoneId] + amount;
    }

    function completeMilestone(uint256 milestoneId) external {
        require(msg.sender == _admin, "Only admin");
        require(milestoneId < _milestoneCount, "Milestone does not exist");
        require(!_milestoneCompleted[milestoneId], "Milestone already completed");
        _milestoneCompleted[milestoneId] = true;
        _completedCount = _completedCount + 1;
    }
}

contract MilestoneTrackerTest is MilestoneTracker {
    constructor() MilestoneTracker() {}

    function initMilestone(uint256 milestoneId) external {
        _milestoneTarget[milestoneId] = 0;
        _milestoneProgress[milestoneId] = 0;
        _milestoneCompleted[milestoneId] = false;
    }
}
