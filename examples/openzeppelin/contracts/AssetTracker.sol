// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Asset lifecycle tracking system.
 * Admin registers assets with a value, transfers ownership via hash,
 * and depreciates asset value over time.
 */
abstract contract AssetTracker {
    address private _admin;
    uint256 private _assetCount;
    uint256 private _totalValue;

    mapping(uint256 => uint256) internal _assetValue;
    mapping(uint256 => uint256) internal _assetOwnerHash;

    constructor() {
        _admin = msg.sender;
        _assetCount = 0;
        _totalValue = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getAssetCount() external view returns (uint256) {
        return _assetCount;
    }

    function getTotalValue() external view returns (uint256) {
        return _totalValue;
    }

    function getAssetValue(uint256 assetId) external view returns (uint256) {
        return _assetValue[assetId];
    }

    function getAssetOwnerHash(uint256 assetId) external view returns (uint256) {
        return _assetOwnerHash[assetId];
    }

    function registerAsset(uint256 assetValue) external returns (uint256) {
        require(msg.sender == _admin, "Not admin");
        uint256 id = _assetCount;
        _assetValue[id] = assetValue;
        _assetOwnerHash[id] = 0;
        _totalValue = _totalValue + assetValue;
        _assetCount = id + 1;
        return id;
    }

    function transferAsset(uint256 assetId, uint256 newOwnerHash) external {
        require(msg.sender == _admin, "Not admin");
        require(assetId < _assetCount, "Asset does not exist");
        _assetOwnerHash[assetId] = newOwnerHash;
    }

    function depreciateAsset(uint256 assetId, uint256 depAmount) external {
        require(msg.sender == _admin, "Not admin");
        require(assetId < _assetCount, "Asset does not exist");
        require(_assetValue[assetId] >= depAmount, "Depreciation exceeds value");
        _assetValue[assetId] = _assetValue[assetId] - depAmount;
        _totalValue = _totalValue - depAmount;
    }
}

contract AssetTrackerTest is AssetTracker {
    constructor() AssetTracker() {}

    function initAsset(uint256 assetId) external {
        _assetValue[assetId] = 0;
        _assetOwnerHash[assetId] = 0;
    }
}
