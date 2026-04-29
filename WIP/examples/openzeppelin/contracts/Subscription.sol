// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Subscription management system.
 * Users subscribe to plans and receive subscription IDs.
 * Admin can be queried; subscriptions can be cancelled.
 */
abstract contract Subscription {
    address private _admin;
    uint256 private _subscriptionCount;
    uint256 private _activeCount;

    mapping(uint256 => uint256) internal _subPlan;
    mapping(uint256 => bool) internal _subActive;
    mapping(uint256 => address) internal _subOwner;

    constructor() {
        _admin = msg.sender;
        _subscriptionCount = 0;
        _activeCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getSubscriptionCount() external view returns (uint256) {
        return _subscriptionCount;
    }

    function getActiveCount() external view returns (uint256) {
        return _activeCount;
    }

    function getSubscriptionPlan(uint256 subId) external view returns (uint256) {
        return _subPlan[subId];
    }

    function isSubscriptionActive(uint256 subId) external view returns (bool) {
        return _subActive[subId];
    }

    function getSubscriptionOwner(uint256 subId) external view returns (address) {
        return _subOwner[subId];
    }

    function subscribe(uint256 planId) external returns (uint256) {
        uint256 id = _subscriptionCount;
        _subPlan[id] = planId;
        _subActive[id] = true;
        _subOwner[id] = msg.sender;
        _subscriptionCount = id + 1;
        _activeCount = _activeCount + 1;
        return id;
    }

    function cancelSubscription(uint256 subId) external {
        require(_subActive[subId] == true, "Not active");
        require(_subOwner[subId] == msg.sender, "Not owner");
        _subActive[subId] = false;
        _activeCount = _activeCount - 1;
    }
}

contract SubscriptionTest is Subscription {
    constructor() Subscription() {}

    function initSubscription(uint256 subId) external {
        _subPlan[subId] = 0;
        _subActive[subId] = false;
        _subOwner[subId] = address(0);
    }
}
