// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Redeemable coupon management system.
 * Admin creates coupons with a value and expiry,
 * then redeems them to track usage and totals.
 */
abstract contract CouponVault {
    address private _admin;
    uint256 private _couponCount;
    uint256 private _redeemedCount;
    uint256 private _totalValue;

    mapping(uint256 => uint256) internal _couponValue;
    mapping(uint256 => bool) internal _couponRedeemed;
    mapping(uint256 => uint256) internal _couponExpiry;

    constructor() {
        _admin = msg.sender;
        _couponCount = 0;
        _redeemedCount = 0;
        _totalValue = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getCouponCount() external view returns (uint256) {
        return _couponCount;
    }

    function getRedeemedCount() external view returns (uint256) {
        return _redeemedCount;
    }

    function getTotalValue() external view returns (uint256) {
        return _totalValue;
    }

    function getCouponValue(uint256 couponId) external view returns (uint256) {
        return _couponValue[couponId];
    }

    function isCouponRedeemed(uint256 couponId) external view returns (bool) {
        return _couponRedeemed[couponId];
    }

    function getCouponExpiry(uint256 couponId) external view returns (uint256) {
        return _couponExpiry[couponId];
    }

    function createCoupon(uint256 value, uint256 expiry) external returns (uint256) {
        require(msg.sender == _admin, "Not admin");
        uint256 id = _couponCount;
        _couponValue[id] = value;
        _couponRedeemed[id] = false;
        _couponExpiry[id] = expiry;
        _totalValue = _totalValue + value;
        _couponCount = id + 1;
        return id;
    }

    function redeemCoupon(uint256 couponId) external {
        require(msg.sender == _admin, "Not admin");
        require(couponId < _couponCount, "Coupon does not exist");
        require(!_couponRedeemed[couponId], "Already redeemed");
        _couponRedeemed[couponId] = true;
        _redeemedCount = _redeemedCount + 1;
    }
}

contract CouponVaultTest is CouponVault {
    constructor() CouponVault() {}

    function initCoupon(uint256 couponId) external {
        _couponValue[couponId] = 0;
        _couponRedeemed[couponId] = false;
        _couponExpiry[couponId] = 0;
    }
}
