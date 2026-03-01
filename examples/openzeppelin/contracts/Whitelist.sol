// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Whitelist access control pattern.
 * Demonstrates mapping-based permission management.
 */
contract WhitelistTest {
    address private _admin;
    mapping(address => bool) private _whitelisted;
    uint256 private _count;

    constructor() {
        _admin = msg.sender;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function isWhitelisted(address account) external view returns (bool) {
        return _whitelisted[account];
    }

    function whitelistCount() external view returns (uint256) {
        return _count;
    }

    function addToWhitelist(address account) external {
        require(msg.sender == _admin, "Whitelist: not admin");
        require(!_whitelisted[account], "Whitelist: already whitelisted");
        _whitelisted[account] = true;
        _count += 1;
    }

    function removeFromWhitelist(address account) external {
        require(msg.sender == _admin, "Whitelist: not admin");
        require(_whitelisted[account], "Whitelist: not whitelisted");
        _whitelisted[account] = false;
        _count -= 1;
    }

    function batchAdd(address a1, address a2) external {
        require(msg.sender == _admin, "Whitelist: not admin");
        if (!_whitelisted[a1]) {
            _whitelisted[a1] = true;
            _count += 1;
        }
        if (!_whitelisted[a2]) {
            _whitelisted[a2] = true;
            _count += 1;
        }
    }
}
