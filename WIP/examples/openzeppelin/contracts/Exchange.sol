// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simple order-book exchange for the Algorand AVM via puya-sol.
 * Admin manages the exchange. Users can place buy/sell orders with price and amount.
 * Orders can be filled or cancelled. Tracks total orders, total volume, fills, and cancels.
 */
abstract contract Exchange {
    address private _admin;
    uint256 private _orderCount;
    uint256 private _totalVolume;
    uint256 private _totalFilled;
    uint256 private _totalCancelled;

    // Order data — keyed by 0-indexed order ID
    mapping(uint256 => address) internal _orderOwner;
    mapping(uint256 => uint256) internal _orderPrice;
    mapping(uint256 => uint256) internal _orderAmount;
    mapping(uint256 => bool) internal _orderIsBuy;
    mapping(uint256 => uint256) internal _orderStatus; // 0=open, 1=filled, 2=cancelled

    constructor() {
        _admin = msg.sender;
    }

    function admin() public view returns (address) {
        return _admin;
    }

    function orderCount() public view returns (uint256) {
        return _orderCount;
    }

    function totalVolume() public view returns (uint256) {
        return _totalVolume;
    }

    function totalFilled() public view returns (uint256) {
        return _totalFilled;
    }

    function totalCancelled() public view returns (uint256) {
        return _totalCancelled;
    }

    function getOrderOwner(uint256 orderId) public view returns (address) {
        return _orderOwner[orderId];
    }

    function getOrderPrice(uint256 orderId) public view returns (uint256) {
        return _orderPrice[orderId];
    }

    function getOrderAmount(uint256 orderId) public view returns (uint256) {
        return _orderAmount[orderId];
    }

    function getOrderIsBuy(uint256 orderId) public view returns (bool) {
        return _orderIsBuy[orderId];
    }

    function getOrderStatus(uint256 orderId) public view returns (uint256) {
        return _orderStatus[orderId];
    }

    function getOrderValue(uint256 orderId) public view returns (uint256) {
        return _orderPrice[orderId] * _orderAmount[orderId];
    }

    function placeOrder(
        address owner,
        uint256 price,
        uint256 amount,
        bool isBuy
    ) public returns (uint256) {
        require(price > 0, "Exchange: price must be > 0");
        require(amount > 0, "Exchange: amount must be > 0");

        uint256 orderId = _orderCount;

        _orderOwner[orderId] = owner;
        _orderPrice[orderId] = price;
        _orderAmount[orderId] = amount;
        _orderIsBuy[orderId] = isBuy;
        _orderStatus[orderId] = 0;

        _totalVolume += price * amount;
        _orderCount += 1;

        return orderId;
    }

    function fillOrder(uint256 orderId) public {
        require(orderId < _orderCount, "Exchange: order does not exist");
        require(_orderStatus[orderId] == 0, "Exchange: order is not open");

        _orderStatus[orderId] = 1;
        _totalFilled += 1;
    }

    function cancelOrder(uint256 orderId) public {
        require(orderId < _orderCount, "Exchange: order does not exist");
        require(_orderStatus[orderId] == 0, "Exchange: order is not open");

        _orderStatus[orderId] = 2;
        _totalCancelled += 1;
    }
}

// Test wrapper — exposes init helpers for AVM box creation
contract ExchangeTest is Exchange {
    // Initialize all mapping boxes for an order ID (AVM boxes must be created before read+write)
    function initOrder(uint256 orderId) external {
        _orderOwner[orderId] = address(0);
        _orderPrice[orderId] = 0;
        _orderAmount[orderId] = 0;
        _orderIsBuy[orderId] = false;
        _orderStatus[orderId] = 0;
    }
}
