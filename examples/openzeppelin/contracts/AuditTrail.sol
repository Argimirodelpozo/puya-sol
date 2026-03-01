// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Audit event recording and querying system.
 * Admin records audit entries with action, actor, and timestamp,
 * then queries individual audit records by ID.
 */
abstract contract AuditTrail {
    address private _admin;
    uint256 private _auditCount;

    mapping(uint256 => uint256) internal _auditAction;
    mapping(uint256 => uint256) internal _auditActor;
    mapping(uint256 => uint256) internal _auditTimestamp;

    constructor() {
        _admin = msg.sender;
        _auditCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getAuditCount() external view returns (uint256) {
        return _auditCount;
    }

    function getAuditAction(uint256 auditId) external view returns (uint256) {
        return _auditAction[auditId];
    }

    function getAuditActor(uint256 auditId) external view returns (uint256) {
        return _auditActor[auditId];
    }

    function getAuditTimestamp(uint256 auditId) external view returns (uint256) {
        return _auditTimestamp[auditId];
    }

    function recordAudit(uint256 action, uint256 actor, uint256 timestamp) external returns (uint256) {
        require(msg.sender == _admin, "Not admin");
        uint256 id = _auditCount;
        _auditAction[id] = action;
        _auditActor[id] = actor;
        _auditTimestamp[id] = timestamp;
        _auditCount = id + 1;
        return id;
    }
}

contract AuditTrailTest is AuditTrail {
    constructor() AuditTrail() {}

    function initAudit(uint256 auditId) external {
        _auditAction[auditId] = 0;
        _auditActor[auditId] = 0;
        _auditTimestamp[auditId] = 0;
    }
}
