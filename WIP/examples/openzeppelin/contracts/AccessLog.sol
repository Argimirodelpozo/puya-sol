// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Access event logging and counting system.
 * Logs access events with resource and timestamp,
 * distinguishing between granted and denied entries.
 */
abstract contract AccessLog {
    address private _admin;
    uint256 private _logCount;
    uint256 private _deniedCount;

    mapping(uint256 => uint256) internal _logTimestamp;
    mapping(uint256 => uint256) internal _logResource;
    mapping(uint256 => bool) internal _logDenied;

    constructor() {
        _admin = msg.sender;
        _logCount = 0;
        _deniedCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getLogCount() external view returns (uint256) {
        return _logCount;
    }

    function getDeniedCount() external view returns (uint256) {
        return _deniedCount;
    }

    function getLogTimestamp(uint256 logId) external view returns (uint256) {
        return _logTimestamp[logId];
    }

    function getLogResource(uint256 logId) external view returns (uint256) {
        return _logResource[logId];
    }

    function isLogDenied(uint256 logId) external view returns (bool) {
        return _logDenied[logId];
    }

    function logAccess(uint256 resource, uint256 timestamp) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        uint256 id = _logCount;
        _logTimestamp[id] = timestamp;
        _logResource[id] = resource;
        _logDenied[id] = false;
        _logCount = id + 1;
        return id;
    }

    function logDenied(uint256 resource, uint256 timestamp) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        uint256 id = _logCount;
        _logTimestamp[id] = timestamp;
        _logResource[id] = resource;
        _logDenied[id] = true;
        _logCount = id + 1;
        _deniedCount = _deniedCount + 1;
        return id;
    }
}

contract AccessLogTest is AccessLog {
    constructor() AccessLog() {}

    function initLog(uint256 logId) external {
        _logTimestamp[logId] = 0;
        _logResource[logId] = 0;
        _logDenied[logId] = false;
    }
}
