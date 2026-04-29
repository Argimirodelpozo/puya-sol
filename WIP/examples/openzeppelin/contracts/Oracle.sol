// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Oracle {
    address public admin;
    uint256 public feedCount;
    uint256 public updateCount;

    mapping(uint256 => uint256) private _feedPrice;
    mapping(uint256 => uint256) private _feedTimestamp;
    mapping(uint256 => bool) private _feedActive;
    mapping(uint256 => uint256) private _feedDecimals;

    constructor() {
        admin = msg.sender;
    }

    function createFeed(uint256 decimals) external returns (uint256) {
        require(msg.sender == admin, "not admin");
        uint256 feedId = feedCount;
        feedCount = feedCount + 1;
        _feedActive[feedId] = true;
        _feedDecimals[feedId] = decimals;
        return feedId;
    }

    function updatePrice(uint256 feedId, uint256 price, uint256 timestamp) external {
        require(msg.sender == admin, "not admin");
        require(_feedActive[feedId], "feed not active");
        _feedPrice[feedId] = price;
        _feedTimestamp[feedId] = timestamp;
        updateCount = updateCount + 1;
    }

    function getPrice(uint256 feedId) external view returns (uint256) {
        return _feedPrice[feedId];
    }

    function getTimestamp(uint256 feedId) external view returns (uint256) {
        return _feedTimestamp[feedId];
    }

    function isFeedActive(uint256 feedId) external view returns (bool) {
        return _feedActive[feedId];
    }

    function getDecimals(uint256 feedId) external view returns (uint256) {
        return _feedDecimals[feedId];
    }

    function deactivateFeed(uint256 feedId) external {
        require(msg.sender == admin, "not admin");
        _feedActive[feedId] = false;
    }

    function activateFeed(uint256 feedId) external {
        require(msg.sender == admin, "not admin");
        _feedActive[feedId] = true;
    }

    function getFeedCount() external view returns (uint256) {
        return feedCount;
    }

    function getUpdateCount() external view returns (uint256) {
        return updateCount;
    }

    function getAdmin() external view returns (address) {
        return admin;
    }

    function isPriceStale(uint256 feedId, uint256 currentTime, uint256 maxAge) external view returns (bool) {
        return currentTime - _feedTimestamp[feedId] > maxAge;
    }
}

contract OracleTest is Oracle {
    constructor() Oracle() {}
}
