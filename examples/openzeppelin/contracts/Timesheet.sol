// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Timesheet / time-tracking system for the Algorand AVM.
 * Admin creates projects, workers log hours to projects.
 * Tracks total projects created and total hours logged globally.
 */
abstract contract Timesheet {
    address private _admin;
    uint256 private _projectCount;
    uint256 private _totalHoursLogged;

    mapping(uint256 => uint256) internal _projectName;
    mapping(uint256 => bool) internal _projectActive;
    mapping(uint256 => uint256) internal _projectHours;
    mapping(address => uint256) internal _workerHours;
    mapping(address => uint256) internal _workerIndex;

    constructor() {
        _admin = msg.sender;
        _projectCount = 0;
        _totalHoursLogged = 0;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function projectCount() external view returns (uint256) {
        return _projectCount;
    }

    function totalHoursLogged() external view returns (uint256) {
        return _totalHoursLogged;
    }

    function getProjectName(uint256 projectId) external view returns (uint256) {
        require(projectId < _projectCount, "Invalid project");
        return _projectName[projectId];
    }

    function isProjectActive(uint256 projectId) external view returns (bool) {
        require(projectId < _projectCount, "Invalid project");
        return _projectActive[projectId];
    }

    function getProjectHours(uint256 projectId) external view returns (uint256) {
        require(projectId < _projectCount, "Invalid project");
        return _projectHours[projectId];
    }

    function getWorkerHours(address worker) external view returns (uint256) {
        return _workerHours[worker];
    }

    function getWorkerIndex(address worker) external view returns (uint256) {
        return _workerIndex[worker];
    }

    function createProject() external returns (uint256) {
        require(msg.sender == _admin, "Not admin");
        uint256 projectId = _projectCount;
        _projectName[projectId] = projectId;
        _projectActive[projectId] = true;
        _projectHours[projectId] = 0;
        _projectCount = projectId + 1;
        return projectId;
    }

    function logHours(uint256 projectId, address worker, uint256 hoursWorked) external {
        require(projectId < _projectCount, "Invalid project");
        require(_projectActive[projectId], "Project not active");
        require(hoursWorked > 0, "Hours must be positive");

        _projectHours[projectId] = _projectHours[projectId] + hoursWorked;
        _workerHours[worker] = _workerHours[worker] + hoursWorked;
        _totalHoursLogged = _totalHoursLogged + hoursWorked;
    }

    function deactivateProject(uint256 projectId) external {
        require(msg.sender == _admin, "Not admin");
        require(projectId < _projectCount, "Invalid project");
        require(_projectActive[projectId], "Already inactive");
        _projectActive[projectId] = false;
    }

    function activateProject(uint256 projectId) external {
        require(msg.sender == _admin, "Not admin");
        require(projectId < _projectCount, "Invalid project");
        require(!_projectActive[projectId], "Already active");
        _projectActive[projectId] = true;
    }
}

// Test contract — standard usage of Timesheet (not part of core source)
contract TimesheetTest is Timesheet {
    function initProject(uint256 projectId) external {
        _projectName[projectId] = 0;
        _projectActive[projectId] = false;
        _projectHours[projectId] = 0;
    }

    function initWorker(address worker) external {
        _workerHours[worker] = 0;
        _workerIndex[worker] = 0;
    }
}
