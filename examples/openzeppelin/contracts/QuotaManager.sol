// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Usage quota tracking and enforcement system.
 * Admin creates quotas with a usage limit, and users consume
 * quota amounts. Tracks remaining quota per entry.
 */
abstract contract QuotaManager {
    address private _admin;
    uint256 private _quotaCount;

    mapping(uint256 => uint256) internal _quotaLimit;
    mapping(uint256 => uint256) internal _quotaUsed;

    constructor() {
        _admin = msg.sender;
        _quotaCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getQuotaCount() external view returns (uint256) {
        return _quotaCount;
    }

    function getQuotaLimit(uint256 quotaId) external view returns (uint256) {
        return _quotaLimit[quotaId];
    }

    function getQuotaUsed(uint256 quotaId) external view returns (uint256) {
        return _quotaUsed[quotaId];
    }

    function getRemaining(uint256 quotaId) external view returns (uint256) {
        require(quotaId < _quotaCount, "Quota does not exist");
        return _quotaLimit[quotaId] - _quotaUsed[quotaId];
    }

    function createQuota(uint256 limit) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        require(limit > 0, "Limit must be positive");
        uint256 id = _quotaCount;
        _quotaLimit[id] = limit;
        _quotaUsed[id] = 0;
        _quotaCount = id + 1;
        return id;
    }

    function useQuota(uint256 quotaId, uint256 amount) external {
        require(quotaId < _quotaCount, "Quota does not exist");
        require(amount > 0, "Amount must be positive");
        uint256 newUsed = _quotaUsed[quotaId] + amount;
        require(newUsed <= _quotaLimit[quotaId], "Quota exceeded");
        _quotaUsed[quotaId] = newUsed;
    }
}

contract QuotaManagerTest is QuotaManager {
    constructor() QuotaManager() {}

    function initQuota(uint256 quotaId) external {
        _quotaLimit[quotaId] = 0;
        _quotaUsed[quotaId] = 0;
    }
}
