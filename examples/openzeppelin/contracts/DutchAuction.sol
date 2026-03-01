// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Dutch auction (descending price auction) contract.
 * The price starts high and decreases linearly over time until a buyer
 * purchases the item or the auction reaches the end price.
 */
abstract contract DutchAuction {
    address private _owner;
    uint256 private _startPrice;
    uint256 private _endPrice;
    uint256 private _startTime;
    uint256 private _duration;
    uint256 private _item;
    bool private _sold;
    address private _buyer;
    uint256 private _finalPrice;
    uint256 private _totalAuctions;

    constructor(
        uint256 startPrice,
        uint256 endPrice,
        uint256 duration,
        uint256 item
    ) {
        require(startPrice > endPrice, "Start price must exceed end price");
        require(duration > 0, "Duration must be positive");

        _owner = msg.sender;
        _startPrice = startPrice;
        _endPrice = endPrice;
        _startTime = 0;
        _duration = duration;
        _item = item;
        _sold = false;
        _buyer = address(0);
        _finalPrice = 0;
        _totalAuctions = 0;
    }

    function startAuction(uint256 currentTime) external {
        require(msg.sender == _owner, "Not owner");
        _startTime = currentTime;
        _sold = false;
        _buyer = address(0);
        _finalPrice = 0;
    }

    function getCurrentPrice(uint256 currentTime) external view returns (uint256) {
        require(_startTime > 0, "Auction not started");
        uint256 elapsed = currentTime - _startTime;
        if (elapsed >= _duration) {
            return _endPrice;
        }
        uint256 priceDrop = (_startPrice - _endPrice) * elapsed / _duration;
        return _startPrice - priceDrop;
    }

    function buy(uint256 currentTime) external returns (uint256) {
        require(!_sold, "Already sold");
        require(_startTime > 0, "Auction not started");

        uint256 elapsed = currentTime - _startTime;
        uint256 price;
        if (elapsed >= _duration) {
            price = _endPrice;
        } else {
            uint256 priceDrop = (_startPrice - _endPrice) * elapsed / _duration;
            price = _startPrice - priceDrop;
        }

        _buyer = msg.sender;
        _finalPrice = price;
        _sold = true;
        _totalAuctions += 1;

        return price;
    }

    function getItem() external view returns (uint256) {
        return _item;
    }

    function isSold() external view returns (bool) {
        return _sold;
    }

    function getBuyer() external view returns (address) {
        return _buyer;
    }

    function getFinalPrice() external view returns (uint256) {
        return _finalPrice;
    }

    function getOwner() external view returns (address) {
        return _owner;
    }

    function getTotalAuctions() external view returns (uint256) {
        return _totalAuctions;
    }
}

/**
 * @dev Test wrapper for DutchAuction with preset parameters:
 * startPrice=10000, endPrice=1000, duration=100, item=42
 */
contract DutchAuctionTest {
    address private _owner;
    uint256 private _startPrice;
    uint256 private _endPrice;
    uint256 private _startTime;
    uint256 private _duration;
    uint256 private _item;
    bool private _sold;
    address private _buyer;
    uint256 private _finalPrice;
    uint256 private _totalAuctions;

    constructor() {
        _owner = msg.sender;
        _startPrice = 10000;
        _endPrice = 1000;
        _startTime = 0;
        _duration = 100;
        _item = 42;
        _sold = false;
        _buyer = address(0);
        _finalPrice = 0;
        _totalAuctions = 0;
    }

    function startAuction(uint256 currentTime) external {
        require(msg.sender == _owner, "Not owner");
        _startTime = currentTime;
        _sold = false;
        _buyer = address(0);
        _finalPrice = 0;
    }

    function getCurrentPrice(uint256 currentTime) external view returns (uint256) {
        require(_startTime > 0, "Auction not started");
        uint256 elapsed = currentTime - _startTime;
        if (elapsed >= _duration) {
            return _endPrice;
        }
        uint256 priceDrop = (_startPrice - _endPrice) * elapsed / _duration;
        return _startPrice - priceDrop;
    }

    function buy(uint256 currentTime) external returns (uint256) {
        require(!_sold, "Already sold");
        require(_startTime > 0, "Auction not started");

        uint256 elapsed = currentTime - _startTime;
        uint256 price;
        if (elapsed >= _duration) {
            price = _endPrice;
        } else {
            uint256 priceDrop = (_startPrice - _endPrice) * elapsed / _duration;
            price = _startPrice - priceDrop;
        }

        _buyer = msg.sender;
        _finalPrice = price;
        _sold = true;
        _totalAuctions += 1;

        return price;
    }

    function getItem() external view returns (uint256) {
        return _item;
    }

    function isSold() external view returns (bool) {
        return _sold;
    }

    function getBuyer() external view returns (address) {
        return _buyer;
    }

    function getFinalPrice() external view returns (uint256) {
        return _finalPrice;
    }

    function getOwner() external view returns (address) {
        return _owner;
    }

    function getTotalAuctions() external view returns (uint256) {
        return _totalAuctions;
    }
}
