// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Allowlist with tiers (different permission levels per address).
 * Tier 0 = not allowed, Tier 1 = basic, Tier 2 = premium, Tier 3 = admin.
 */
contract AllowlistTest {
    address private _owner;
    uint256 private _memberCount;

    mapping(address => uint256) private _tiers;

    constructor() {
        _owner = msg.sender;
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function memberCount() external view returns (uint256) {
        return _memberCount;
    }

    function tierOf(address account) external view returns (uint256) {
        return _tiers[account];
    }

    function isAllowed(address account) external view returns (bool) {
        return _tiers[account] > 0;
    }

    function isPremium(address account) external view returns (bool) {
        return _tiers[account] >= 2;
    }

    function isAdmin(address account) external view returns (bool) {
        return _tiers[account] >= 3;
    }

    function setTier(address account, uint256 tier) external {
        require(msg.sender == _owner, "Not owner");
        uint256 oldTier = _tiers[account];

        if (oldTier == 0 && tier > 0) {
            _memberCount += 1;
        } else if (oldTier > 0 && tier == 0) {
            _memberCount -= 1;
        }

        _tiers[account] = tier;
    }

    function removeMember(address account) external {
        require(msg.sender == _owner, "Not owner");
        if (_tiers[account] > 0) {
            _memberCount -= 1;
        }
        _tiers[account] = 0;
    }
}
