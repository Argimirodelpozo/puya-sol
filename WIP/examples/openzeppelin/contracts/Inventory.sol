// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Inventory {
    address private _admin;
    uint256 private _itemCount;
    uint256 private _totalItems;
    uint256 private _lowStockThreshold;

    mapping(uint256 => uint256) internal _itemQuantity;
    mapping(uint256 => uint256) internal _itemPrice;
    mapping(uint256 => bool) internal _itemActive;
    mapping(uint256 => uint256) internal _itemSold;

    constructor(uint256 lowStockThreshold_) {
        _admin = msg.sender;
        _itemCount = 0;
        _totalItems = 0;
        _lowStockThreshold = lowStockThreshold_;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getItemCount() external view returns (uint256) {
        return _itemCount;
    }

    function getTotalItems() external view returns (uint256) {
        return _totalItems;
    }

    function getLowStockThreshold() external view returns (uint256) {
        return _lowStockThreshold;
    }

    function getItemQuantity(uint256 itemId) external view returns (uint256) {
        return _itemQuantity[itemId];
    }

    function getItemPrice(uint256 itemId) external view returns (uint256) {
        return _itemPrice[itemId];
    }

    function isItemActive(uint256 itemId) external view returns (bool) {
        return _itemActive[itemId];
    }

    function getItemSold(uint256 itemId) external view returns (uint256) {
        return _itemSold[itemId];
    }

    function getItemValue(uint256 itemId) external view returns (uint256) {
        return _itemQuantity[itemId] * _itemPrice[itemId];
    }

    function isLowStock(uint256 itemId) external view returns (bool) {
        return _itemQuantity[itemId] < _lowStockThreshold;
    }

    function addItem(uint256 quantity, uint256 price) external returns (uint256) {
        uint256 itemId = _itemCount;
        _itemQuantity[itemId] = quantity;
        _itemPrice[itemId] = price;
        _itemActive[itemId] = true;
        _itemSold[itemId] = 0;
        _itemCount = itemId + 1;
        _totalItems = _totalItems + quantity;
        return itemId;
    }

    function restock(uint256 itemId, uint256 quantity) external {
        require(_itemActive[itemId], "Item is not active");
        _itemQuantity[itemId] = _itemQuantity[itemId] + quantity;
        _totalItems = _totalItems + quantity;
    }

    function sell(uint256 itemId, uint256 quantity) external {
        require(_itemActive[itemId], "Item is not active");
        require(quantity <= _itemQuantity[itemId], "Insufficient stock");
        _itemQuantity[itemId] = _itemQuantity[itemId] - quantity;
        _itemSold[itemId] = _itemSold[itemId] + quantity;
        _totalItems = _totalItems - quantity;
    }

    function discontinue(uint256 itemId) external {
        _itemActive[itemId] = false;
    }

    function reactivate(uint256 itemId) external {
        _itemActive[itemId] = true;
    }

    function setPrice(uint256 itemId, uint256 newPrice) external {
        _itemPrice[itemId] = newPrice;
    }

    function setLowStockThreshold(uint256 newThreshold) external {
        require(msg.sender == _admin, "Only admin");
        _lowStockThreshold = newThreshold;
    }
}

contract InventoryTest is Inventory {
    constructor() Inventory(5) {}

    function initItem(uint256 itemId) external {
        _itemQuantity[itemId] = 0;
        _itemPrice[itemId] = 0;
        _itemActive[itemId] = false;
        _itemSold[itemId] = 0;
    }
}
