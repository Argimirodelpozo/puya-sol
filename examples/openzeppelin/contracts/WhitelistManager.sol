// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Address whitelist management system on the Algorand AVM.
 * Admin adds/removes address hashes from the whitelist.
 * Uses uint256 address hashes for AVM compatibility.
 */
abstract contract WhitelistManager {
    address private _admin;
    uint256 private _whitelistCount;
    uint256 private _batchOpCount;

    mapping(uint256 => bool) internal _whitelisted;

    constructor() {
        _admin = msg.sender;
        _whitelistCount = 0;
        _batchOpCount = 0;
    }

    function getAdmin() public view returns (address) {
        return _admin;
    }

    function getWhitelistCount() public view returns (uint256) {
        return _whitelistCount;
    }

    function getBatchOpCount() public view returns (uint256) {
        return _batchOpCount;
    }

    function isWhitelisted(uint256 addrHash) public view returns (bool) {
        return _whitelisted[addrHash];
    }

    function addToWhitelist(uint256 addrHash) public {
        require(msg.sender == _admin, "WhitelistManager: not admin");
        require(!_whitelisted[addrHash], "WhitelistManager: already whitelisted");
        _whitelisted[addrHash] = true;
        _whitelistCount = _whitelistCount + 1;
    }

    function removeFromWhitelist(uint256 addrHash) public {
        require(msg.sender == _admin, "WhitelistManager: not admin");
        require(_whitelisted[addrHash], "WhitelistManager: not whitelisted");
        _whitelisted[addrHash] = false;
        _whitelistCount = _whitelistCount - 1;
    }

    function batchAdd(uint256 addrHash1, uint256 addrHash2) public {
        require(msg.sender == _admin, "WhitelistManager: not admin");
        if (!_whitelisted[addrHash1]) {
            _whitelisted[addrHash1] = true;
            _whitelistCount = _whitelistCount + 1;
        }
        if (!_whitelisted[addrHash2]) {
            _whitelisted[addrHash2] = true;
            _whitelistCount = _whitelistCount + 1;
        }
        _batchOpCount = _batchOpCount + 1;
    }

    function batchRemove(uint256 addrHash1, uint256 addrHash2) public {
        require(msg.sender == _admin, "WhitelistManager: not admin");
        if (_whitelisted[addrHash1]) {
            _whitelisted[addrHash1] = false;
            _whitelistCount = _whitelistCount - 1;
        }
        if (_whitelisted[addrHash2]) {
            _whitelisted[addrHash2] = false;
            _whitelistCount = _whitelistCount - 1;
        }
        _batchOpCount = _batchOpCount + 1;
    }
}

contract WhitelistManagerTest is WhitelistManager {
    constructor() WhitelistManager() {}

    function initWhitelistEntry(uint256 addrHash) public {
        _whitelisted[addrHash] = false;
    }
}
