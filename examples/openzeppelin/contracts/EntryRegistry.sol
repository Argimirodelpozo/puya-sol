// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Generic registry for named entries on the Algorand AVM.
 * Admin registers entries with name hashes, tracks ownership,
 * and supports activation/deactivation and ownership transfer.
 */
abstract contract EntryRegistry {
    address private _admin;
    uint256 private _entryCount;

    mapping(uint256 => address) internal _entryOwner;
    mapping(uint256 => bool) internal _entryActive;
    mapping(uint256 => uint256) internal _entryNameHash;

    constructor() {
        _admin = msg.sender;
        _entryCount = 0;
    }

    function getAdmin() public view returns (address) {
        return _admin;
    }

    function getEntryCount() public view returns (uint256) {
        return _entryCount;
    }

    function getEntryOwner(uint256 entryId) public view returns (address) {
        return _entryOwner[entryId];
    }

    function isEntryActive(uint256 entryId) public view returns (bool) {
        return _entryActive[entryId];
    }

    function getEntryNameHash(uint256 entryId) public view returns (uint256) {
        return _entryNameHash[entryId];
    }

    function registerEntry(uint256 nameHash) public returns (uint256) {
        uint256 entryId = _entryCount;
        _entryOwner[entryId] = msg.sender;
        _entryActive[entryId] = true;
        _entryNameHash[entryId] = nameHash;
        _entryCount = entryId + 1;
        return entryId;
    }

    function deactivateEntry(uint256 entryId) public {
        require(entryId < _entryCount, "EntryRegistry: entry does not exist");
        require(_entryActive[entryId], "EntryRegistry: entry already inactive");
        require(
            msg.sender == _admin || msg.sender == _entryOwner[entryId],
            "EntryRegistry: not authorized"
        );
        _entryActive[entryId] = false;
    }

    function activateEntry(uint256 entryId) public {
        require(entryId < _entryCount, "EntryRegistry: entry does not exist");
        require(!_entryActive[entryId], "EntryRegistry: entry already active");
        require(
            msg.sender == _admin || msg.sender == _entryOwner[entryId],
            "EntryRegistry: not authorized"
        );
        _entryActive[entryId] = true;
    }

    function transferEntry(uint256 entryId, address newOwner) public {
        require(msg.sender == _admin, "EntryRegistry: not admin");
        require(entryId < _entryCount, "EntryRegistry: entry does not exist");
        _entryOwner[entryId] = newOwner;
    }
}

contract EntryRegistryTest is EntryRegistry {
    constructor() EntryRegistry() {}

    function initEntry(uint256 entryId) public {
        _entryOwner[entryId] = address(0);
        _entryActive[entryId] = false;
        _entryNameHash[entryId] = 0;
    }
}
