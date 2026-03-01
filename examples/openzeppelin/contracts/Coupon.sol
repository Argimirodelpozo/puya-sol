// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Coupon {
    address private _admin;
    uint256 private _couponCount;
    uint256 private _totalRedemptions;

    mapping(uint256 => uint256) internal _couponDiscount;
    mapping(uint256 => uint256) internal _couponMaxUses;
    mapping(uint256 => uint256) internal _couponUseCount;
    mapping(uint256 => bool) internal _couponActive;
    mapping(bytes32 => bool) internal _hasRedeemed;

    constructor() {
        _admin = msg.sender;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getCouponCount() external view returns (uint256) {
        return _couponCount;
    }

    function getTotalRedemptions() external view returns (uint256) {
        return _totalRedemptions;
    }

    function getCouponDiscount(uint256 couponId) external view returns (uint256) {
        return _couponDiscount[couponId];
    }

    function getCouponMaxUses(uint256 couponId) external view returns (uint256) {
        return _couponMaxUses[couponId];
    }

    function getCouponUseCount(uint256 couponId) external view returns (uint256) {
        return _couponUseCount[couponId];
    }

    function isCouponActive(uint256 couponId) external view returns (bool) {
        return _couponActive[couponId];
    }

    function hasRedeemed(address user, uint256 couponId) external view returns (bool) {
        bytes32 key = keccak256(abi.encodePacked(user, couponId));
        return _hasRedeemed[key];
    }

    function createCoupon(uint256 discount, uint256 maxUses) external returns (uint256) {
        require(msg.sender == _admin, "only admin");
        require(discount > 0, "discount must be positive");
        require(maxUses > 0, "maxUses must be positive");

        uint256 couponId = _couponCount;
        _couponDiscount[couponId] = discount;
        _couponMaxUses[couponId] = maxUses;
        _couponUseCount[couponId] = 0;
        _couponActive[couponId] = true;
        _couponCount = couponId + 1;

        return couponId;
    }

    function redeemCoupon(address user, uint256 couponId) external {
        require(_couponActive[couponId], "coupon not active");
        require(_couponUseCount[couponId] < _couponMaxUses[couponId], "coupon max uses reached");

        bytes32 key = keccak256(abi.encodePacked(user, couponId));
        require(!_hasRedeemed[key], "already redeemed");

        _hasRedeemed[key] = true;
        _couponUseCount[couponId] = _couponUseCount[couponId] + 1;
        _totalRedemptions = _totalRedemptions + 1;
    }

    function deactivateCoupon(uint256 couponId) external {
        require(msg.sender == _admin, "only admin");
        _couponActive[couponId] = false;
    }

    function activateCoupon(uint256 couponId) external {
        require(msg.sender == _admin, "only admin");
        _couponActive[couponId] = true;
    }
}

contract CouponTest is Coupon {
    constructor() Coupon() {}

    function initCoupon(uint256 couponId) external {
        _couponDiscount[couponId] = 0;
        _couponMaxUses[couponId] = 0;
        _couponUseCount[couponId] = 0;
        _couponActive[couponId] = false;
    }

    function initRedemption(address user, uint256 couponId) external {
        bytes32 key = keccak256(abi.encodePacked(user, couponId));
        _hasRedeemed[key] = false;
    }
}
