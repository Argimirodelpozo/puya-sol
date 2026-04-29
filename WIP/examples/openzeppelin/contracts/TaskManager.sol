// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract TaskManager {
    address private _admin;
    uint256 private _taskCount;
    uint256 private _completedCount;
    uint256 private _totalEffort;

    mapping(uint256 => uint256) internal _taskPriority;
    mapping(uint256 => uint256) internal _taskEffort;
    mapping(uint256 => address) internal _taskAssignee;
    mapping(uint256 => uint256) internal _taskStatus;
    mapping(uint256 => bool) internal _taskExists;

    constructor() {
        _admin = msg.sender;
        _taskCount = 0;
        _completedCount = 0;
        _totalEffort = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getTaskCount() external view returns (uint256) {
        return _taskCount;
    }

    function getCompletedCount() external view returns (uint256) {
        return _completedCount;
    }

    function getTotalEffort() external view returns (uint256) {
        return _totalEffort;
    }

    function getTaskPriority(uint256 taskId) external view returns (uint256) {
        return _taskPriority[taskId];
    }

    function getTaskEffort(uint256 taskId) external view returns (uint256) {
        return _taskEffort[taskId];
    }

    function getTaskAssignee(uint256 taskId) external view returns (address) {
        return _taskAssignee[taskId];
    }

    function getTaskStatus(uint256 taskId) external view returns (uint256) {
        return _taskStatus[taskId];
    }

    function taskExists(uint256 taskId) external view returns (bool) {
        return _taskExists[taskId];
    }

    function createTask(uint256 priority, uint256 effort) external returns (uint256) {
        require(priority <= 2, "Invalid priority");
        uint256 taskId = _taskCount;
        _taskPriority[taskId] = priority;
        _taskEffort[taskId] = effort;
        _taskAssignee[taskId] = address(0);
        _taskStatus[taskId] = 0;
        _taskExists[taskId] = true;
        _totalEffort = _totalEffort + effort;
        _taskCount = _taskCount + 1;
        return taskId;
    }

    function assignTask(uint256 taskId, address assignee) external {
        require(_taskExists[taskId] == true, "Task does not exist");
        require(_taskStatus[taskId] == 0, "Task not open");
        _taskAssignee[taskId] = assignee;
        _taskStatus[taskId] = 1;
    }

    function completeTask(uint256 taskId) external {
        require(_taskExists[taskId] == true, "Task does not exist");
        require(_taskStatus[taskId] == 1, "Task not assigned");
        _taskStatus[taskId] = 2;
        _completedCount = _completedCount + 1;
    }

    function isComplete(uint256 taskId) external view returns (bool) {
        return _taskStatus[taskId] == 2;
    }
}

contract TaskManagerTest is TaskManager {
    constructor() TaskManager() {}

    function initTask(uint256 taskId) external {
        _taskPriority[taskId] = 0;
        _taskEffort[taskId] = 0;
        _taskAssignee[taskId] = address(0);
        _taskStatus[taskId] = 0;
        _taskExists[taskId] = false;
    }
}
