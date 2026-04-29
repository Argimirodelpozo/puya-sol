// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Compliance/Regulatory tracking system for the Algorand AVM.
 * Admin creates compliance rules with severity levels, checks entities
 * against rules, and tracks violations.
 */
abstract contract Compliance {
    address private _admin;
    uint256 private _ruleCount;
    uint256 private _checkCount;
    uint256 private _totalViolations;

    mapping(uint256 => uint256) internal _ruleSeverity;
    mapping(uint256 => bool) internal _ruleActive;
    mapping(uint256 => uint256) internal _ruleViolationCount;
    mapping(address => uint256) internal _entityViolations;
    mapping(address => uint256) internal _entityChecks;

    constructor() {
        _admin = msg.sender;
    }

    function getAdmin() public view returns (address) {
        return _admin;
    }

    function getRuleCount() public view returns (uint256) {
        return _ruleCount;
    }

    function getCheckCount() public view returns (uint256) {
        return _checkCount;
    }

    function getTotalViolations() public view returns (uint256) {
        return _totalViolations;
    }

    function getRuleSeverity(uint256 ruleId) public view returns (uint256) {
        return _ruleSeverity[ruleId];
    }

    function isRuleActive(uint256 ruleId) public view returns (bool) {
        return _ruleActive[ruleId];
    }

    function getRuleViolationCount(uint256 ruleId) public view returns (uint256) {
        return _ruleViolationCount[ruleId];
    }

    function getEntityViolations(address entity) public view returns (uint256) {
        return _entityViolations[entity];
    }

    function getEntityChecks(address entity) public view returns (uint256) {
        return _entityChecks[entity];
    }

    function createRule(uint256 severity) public returns (uint256) {
        require(msg.sender == _admin, "Compliance: not admin");
        require(severity <= 2, "Compliance: invalid severity");
        uint256 ruleId = _ruleCount;
        _ruleSeverity[ruleId] = severity;
        _ruleActive[ruleId] = true;
        _ruleViolationCount[ruleId] = 0;
        _ruleCount = ruleId + 1;
        return ruleId;
    }

    function performCheck(address entity, uint256 ruleId, bool passed) public {
        require(msg.sender == _admin, "Compliance: not admin");
        require(ruleId < _ruleCount, "Compliance: rule does not exist");
        require(_ruleActive[ruleId], "Compliance: rule not active");
        _checkCount = _checkCount + 1;
        _entityChecks[entity] = _entityChecks[entity] + 1;
        if (!passed) {
            _totalViolations = _totalViolations + 1;
            _entityViolations[entity] = _entityViolations[entity] + 1;
            _ruleViolationCount[ruleId] = _ruleViolationCount[ruleId] + 1;
        }
    }

    function deactivateRule(uint256 ruleId) public {
        require(msg.sender == _admin, "Compliance: not admin");
        require(ruleId < _ruleCount, "Compliance: rule does not exist");
        require(_ruleActive[ruleId], "Compliance: rule already inactive");
        _ruleActive[ruleId] = false;
    }

    function activateRule(uint256 ruleId) public {
        require(msg.sender == _admin, "Compliance: not admin");
        require(ruleId < _ruleCount, "Compliance: rule does not exist");
        require(!_ruleActive[ruleId], "Compliance: rule already active");
        _ruleActive[ruleId] = true;
    }

    function isEntityCompliant(address entity) public view returns (bool) {
        return _entityViolations[entity] == 0;
    }
}

contract ComplianceTest is Compliance {
    constructor() Compliance() {}

    function initRule(uint256 ruleId) public {
        _ruleSeverity[ruleId] = 0;
        _ruleActive[ruleId] = false;
        _ruleViolationCount[ruleId] = 0;
    }

    function initEntity(address entity) public {
        _entityViolations[entity] = 0;
        _entityChecks[entity] = 0;
    }
}
