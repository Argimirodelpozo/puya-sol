// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Auction2 {
    address private _admin;
    uint256 private _lotCount;
    uint256 private _totalBids;
    uint256 private _totalRevenue;

    mapping(uint256 => uint256) internal _lotReserve;
    mapping(uint256 => uint256) internal _lotHighBid;
    mapping(uint256 => address) internal _lotHighBidder;
    mapping(uint256 => bool) internal _lotClosed;
    mapping(uint256 => uint256) internal _lotBidCount;

    constructor() {
        _admin = msg.sender;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function lotCount() external view returns (uint256) {
        return _lotCount;
    }

    function totalBids() external view returns (uint256) {
        return _totalBids;
    }

    function totalRevenue() external view returns (uint256) {
        return _totalRevenue;
    }

    function getLotReserve(uint256 lotId) external view returns (uint256) {
        return _lotReserve[lotId];
    }

    function getLotHighBid(uint256 lotId) external view returns (uint256) {
        return _lotHighBid[lotId];
    }

    function getLotHighBidder(uint256 lotId) external view returns (address) {
        return _lotHighBidder[lotId];
    }

    function isLotClosed(uint256 lotId) external view returns (bool) {
        return _lotClosed[lotId];
    }

    function getLotBidCount(uint256 lotId) external view returns (uint256) {
        return _lotBidCount[lotId];
    }

    function createLot(uint256 reserve) external returns (uint256) {
        uint256 lotId = _lotCount;
        _lotReserve[lotId] = reserve;
        _lotHighBid[lotId] = 0;
        _lotHighBidder[lotId] = address(0);
        _lotClosed[lotId] = false;
        _lotBidCount[lotId] = 0;
        _lotCount = lotId + 1;
        return lotId;
    }

    function placeBid(uint256 lotId, address bidder, uint256 amount) external {
        require(!_lotClosed[lotId], "Lot is closed");
        require(amount >= _lotReserve[lotId], "Bid below reserve");
        require(amount > _lotHighBid[lotId], "Bid not high enough");

        _lotHighBid[lotId] = amount;
        _lotHighBidder[lotId] = bidder;
        _lotBidCount[lotId] = _lotBidCount[lotId] + 1;
        _totalBids = _totalBids + 1;
    }

    function closeLot(uint256 lotId) external {
        require(!_lotClosed[lotId], "Lot already closed");
        _lotClosed[lotId] = true;
        uint256 highBid = _lotHighBid[lotId];
        if (highBid > 0) {
            _totalRevenue = _totalRevenue + highBid;
        }
    }
}

contract Auction2Test is Auction2 {
    constructor() Auction2() {}

    function initLot(uint256 lotId) external {
        _lotReserve[lotId] = 0;
        _lotHighBid[lotId] = 0;
        _lotHighBidder[lotId] = address(0);
        _lotClosed[lotId] = false;
        _lotBidCount[lotId] = 0;
    }
}
