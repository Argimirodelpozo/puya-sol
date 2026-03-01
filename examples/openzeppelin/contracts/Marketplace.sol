// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simple marketplace for listing and buying items.
 * Items are identified by uint256 IDs, each has a seller, price, and sold status.
 */
contract MarketplaceTest {
    address private _owner;
    uint256 private _itemCount;
    uint256 private _feePercent; // in basis points (100 = 1%)
    uint256 private _totalFees;

    mapping(uint256 => address) private _itemSeller;
    mapping(uint256 => uint256) private _itemPrice;
    mapping(uint256 => bool) private _itemSold;

    constructor() {
        _owner = msg.sender;
        _feePercent = 250; // 2.5%
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function itemCount() external view returns (uint256) {
        return _itemCount;
    }

    function feePercent() external view returns (uint256) {
        return _feePercent;
    }

    function totalFees() external view returns (uint256) {
        return _totalFees;
    }

    function setFeePercent(uint256 fee) external {
        require(msg.sender == _owner, "Not owner");
        require(fee <= 1000, "Fee too high"); // max 10%
        _feePercent = fee;
    }

    function listItem(address seller, uint256 price) external returns (uint256) {
        require(price > 0, "Price must be > 0");

        _itemCount += 1;
        uint256 id = _itemCount;

        _itemSeller[id] = seller;
        _itemPrice[id] = price;
        _itemSold[id] = false;

        return id;
    }

    function getItemSeller(uint256 itemId) external view returns (address) {
        return _itemSeller[itemId];
    }

    function getItemPrice(uint256 itemId) external view returns (uint256) {
        return _itemPrice[itemId];
    }

    function isItemSold(uint256 itemId) external view returns (bool) {
        return _itemSold[itemId];
    }

    function calculateFee(uint256 price) external view returns (uint256) {
        return (price * _feePercent) / 10000;
    }

    function buyItem(uint256 itemId) external returns (bool) {
        require(_itemSeller[itemId] != address(0), "Item not found");
        require(!_itemSold[itemId], "Already sold");

        uint256 price = _itemPrice[itemId];
        uint256 fee = (price * _feePercent) / 10000;
        _totalFees += fee;

        _itemSold[itemId] = true;
        return true;
    }

    function cancelItem(uint256 itemId, address caller) external {
        require(_itemSeller[itemId] == caller, "Not seller");
        require(!_itemSold[itemId], "Already sold");

        _itemSeller[itemId] = address(0);
        _itemPrice[itemId] = 0;
    }
}
