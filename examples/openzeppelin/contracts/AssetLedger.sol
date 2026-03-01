// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Asset ownership ledger with transfer tracking.
 * Admin creates assets with an owner and value,
 * then transfers ownership while counting transfers per asset.
 */
abstract contract AssetLedger {
    address private _admin;
    uint256 private _assetCount;
    uint256 private _totalTransfers;

    mapping(uint256 => uint256) internal _assetOwner;
    mapping(uint256 => uint256) internal _assetValue;
    mapping(uint256 => uint256) internal _transferCount;

    constructor() {
        _admin = msg.sender;
        _assetCount = 0;
        _totalTransfers = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getAssetCount() external view returns (uint256) {
        return _assetCount;
    }

    function getTotalTransfers() external view returns (uint256) {
        return _totalTransfers;
    }

    function getAssetOwner(uint256 assetId) external view returns (uint256) {
        return _assetOwner[assetId];
    }

    function getAssetValue(uint256 assetId) external view returns (uint256) {
        return _assetValue[assetId];
    }

    function getTransferCount(uint256 assetId) external view returns (uint256) {
        return _transferCount[assetId];
    }

    function createAsset(uint256 owner, uint256 value) external returns (uint256) {
        require(msg.sender == _admin, "Not admin");
        uint256 id = _assetCount;
        _assetOwner[id] = owner;
        _assetValue[id] = value;
        _transferCount[id] = 0;
        _assetCount = id + 1;
        return id;
    }

    function transferAsset(uint256 assetId, uint256 newOwner) external {
        require(msg.sender == _admin, "Not admin");
        require(assetId < _assetCount, "Asset does not exist");
        _assetOwner[assetId] = newOwner;
        _transferCount[assetId] = _transferCount[assetId] + 1;
        _totalTransfers = _totalTransfers + 1;
    }
}

contract AssetLedgerTest is AssetLedger {
    constructor() AssetLedger() {}

    function initAsset(uint256 assetId) external {
        _assetOwner[assetId] = 0;
        _assetValue[assetId] = 0;
        _transferCount[assetId] = 0;
    }
}
