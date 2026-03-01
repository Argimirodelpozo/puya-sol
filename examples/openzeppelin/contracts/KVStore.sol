// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract KVStore {
    address private _admin;
    uint256 private _entryCount;
    uint256 private _version;
    uint256 private _totalWrites;
    uint256 private _totalDeletes;

    mapping(uint256 => uint256) internal _values;
    mapping(uint256 => bool) internal _exists;
    mapping(uint256 => uint256) internal _lastModified;

    constructor() {
        _admin = msg.sender;
        _entryCount = 0;
        _version = 0;
        _totalWrites = 0;
        _totalDeletes = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getEntryCount() external view returns (uint256) {
        return _entryCount;
    }

    function getVersion() external view returns (uint256) {
        return _version;
    }

    function getTotalWrites() external view returns (uint256) {
        return _totalWrites;
    }

    function getTotalDeletes() external view returns (uint256) {
        return _totalDeletes;
    }

    function getValue(uint256 key) external view returns (uint256) {
        return _values[key];
    }

    function exists(uint256 key) external view returns (bool) {
        return _exists[key];
    }

    function getLastModified(uint256 key) external view returns (uint256) {
        return _lastModified[key];
    }

    function setValue(uint256 key, uint256 value) external {
        if (!_exists[key]) {
            _entryCount = _entryCount + 1;
        }
        _values[key] = value;
        _exists[key] = true;
        _lastModified[key] = _version;
        _version = _version + 1;
        _totalWrites = _totalWrites + 1;
    }

    function deleteKey(uint256 key) external {
        require(_exists[key], "key does not exist");
        _values[key] = 0;
        _exists[key] = false;
        _entryCount = _entryCount - 1;
        _version = _version + 1;
        _totalDeletes = _totalDeletes + 1;
    }

    function batchSet(uint256 key1, uint256 value1, uint256 key2, uint256 value2) external {
        if (!_exists[key1]) {
            _entryCount = _entryCount + 1;
        }
        _values[key1] = value1;
        _exists[key1] = true;
        _lastModified[key1] = _version;
        _version = _version + 1;
        _totalWrites = _totalWrites + 1;

        if (!_exists[key2]) {
            _entryCount = _entryCount + 1;
        }
        _values[key2] = value2;
        _exists[key2] = true;
        _lastModified[key2] = _version;
        _version = _version + 1;
        _totalWrites = _totalWrites + 1;
    }
}

contract KVStoreTest is KVStore {
    constructor() KVStore() {}

    function initKey(uint256 key) external {
        _values[key] = 0;
        _exists[key] = false;
        _lastModified[key] = 0;
    }
}
