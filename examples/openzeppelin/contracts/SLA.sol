// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract SLA {
    address private _admin;
    uint256 private _slaCount;
    uint256 private _reportCount;
    uint256 private _totalBreaches;

    mapping(uint256 => uint256) internal _slaTarget;
    mapping(uint256 => uint256) internal _slaReportCount;
    mapping(uint256 => uint256) internal _slaBreachCount;
    mapping(uint256 => bool) internal _slaActive;
    mapping(uint256 => uint256) internal _slaLastUptime;

    constructor() {
        _admin = msg.sender;
        _slaCount = 0;
        _reportCount = 0;
        _totalBreaches = 0;
    }

    function getAdmin() public view returns (address) {
        return _admin;
    }

    function getSlaCount() public view returns (uint256) {
        return _slaCount;
    }

    function getReportCount() public view returns (uint256) {
        return _reportCount;
    }

    function getTotalBreaches() public view returns (uint256) {
        return _totalBreaches;
    }

    function getSlaTarget(uint256 slaId) public view returns (uint256) {
        return _slaTarget[slaId];
    }

    function getSlaReportCount(uint256 slaId) public view returns (uint256) {
        return _slaReportCount[slaId];
    }

    function getSlaBreachCount(uint256 slaId) public view returns (uint256) {
        return _slaBreachCount[slaId];
    }

    function isSlaActive(uint256 slaId) public view returns (bool) {
        return _slaActive[slaId];
    }

    function getSlaLastUptime(uint256 slaId) public view returns (uint256) {
        return _slaLastUptime[slaId];
    }

    function createSLA(uint256 target) public returns (uint256) {
        require(msg.sender == _admin, "Not admin");
        require(target <= 10000, "Target exceeds basis points max");
        uint256 slaId = _slaCount;
        _slaTarget[slaId] = target;
        _slaReportCount[slaId] = 0;
        _slaBreachCount[slaId] = 0;
        _slaActive[slaId] = true;
        _slaLastUptime[slaId] = 0;
        _slaCount = _slaCount + 1;
        return slaId;
    }

    function fileReport(uint256 slaId, uint256 actualUptime) public {
        require(slaId < _slaCount, "SLA does not exist");
        require(_slaActive[slaId] == true, "SLA not active");
        _slaReportCount[slaId] = _slaReportCount[slaId] + 1;
        _reportCount = _reportCount + 1;
        _slaLastUptime[slaId] = actualUptime;
        if (actualUptime < _slaTarget[slaId]) {
            _slaBreachCount[slaId] = _slaBreachCount[slaId] + 1;
            _totalBreaches = _totalBreaches + 1;
        }
    }

    function deactivateSLA(uint256 slaId) public {
        require(msg.sender == _admin, "Not admin");
        require(slaId < _slaCount, "SLA does not exist");
        _slaActive[slaId] = false;
    }

    function activateSLA(uint256 slaId) public {
        require(msg.sender == _admin, "Not admin");
        require(slaId < _slaCount, "SLA does not exist");
        _slaActive[slaId] = true;
    }

    function isSLABreached(uint256 slaId) public view returns (bool) {
        uint256 breachCount = _slaBreachCount[slaId];
        bool breached = breachCount > 0;
        return breached;
    }
}

contract SLATest is SLA {
    constructor() SLA() {}

    function initSLA(uint256 slaId) external {
        _slaTarget[slaId] = 0;
        _slaReportCount[slaId] = 0;
        _slaBreachCount[slaId] = 0;
        _slaActive[slaId] = false;
        _slaLastUptime[slaId] = 0;
    }
}
