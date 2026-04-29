// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Two-party escrow with arbiter. Supports multiple escrow deals.
 * Each deal has a buyer, seller, amount, and status.
 */
contract Escrow2PartyTest {
    address private _arbiter;
    uint256 private _dealCount;
    uint256 private _feeRate; // basis points
    uint256 private _feesCollected;

    mapping(uint256 => address) private _dealBuyer;
    mapping(uint256 => address) private _dealSeller;
    mapping(uint256 => uint256) private _dealAmount;
    mapping(uint256 => uint256) private _dealStatus; // 0=pending, 1=released, 2=refunded, 3=disputed

    constructor() {
        _arbiter = msg.sender;
        _feeRate = 100; // 1%
    }

    function arbiter() external view returns (address) {
        return _arbiter;
    }

    function dealCount() external view returns (uint256) {
        return _dealCount;
    }

    function feeRate() external view returns (uint256) {
        return _feeRate;
    }

    function feesCollected() external view returns (uint256) {
        return _feesCollected;
    }

    function createDeal(address buyer, address seller, uint256 amount) external returns (uint256) {
        require(amount > 0, "Amount must be > 0");
        _dealCount += 1;
        uint256 id = _dealCount;
        _dealBuyer[id] = buyer;
        _dealSeller[id] = seller;
        _dealAmount[id] = amount;
        _dealStatus[id] = 0; // pending
        return id;
    }

    function getDealBuyer(uint256 dealId) external view returns (address) {
        return _dealBuyer[dealId];
    }

    function getDealSeller(uint256 dealId) external view returns (address) {
        return _dealSeller[dealId];
    }

    function getDealAmount(uint256 dealId) external view returns (uint256) {
        return _dealAmount[dealId];
    }

    function getDealStatus(uint256 dealId) external view returns (uint256) {
        return _dealStatus[dealId];
    }

    function releaseFunds(uint256 dealId) external {
        require(_dealStatus[dealId] == 0, "Deal not pending");
        uint256 amount = _dealAmount[dealId];
        uint256 fee = (amount * _feeRate) / 10000;
        _feesCollected += fee;
        _dealStatus[dealId] = 1; // released
    }

    function refund(uint256 dealId) external {
        require(_dealStatus[dealId] == 0, "Deal not pending");
        _dealStatus[dealId] = 2; // refunded
    }

    function dispute(uint256 dealId) external {
        require(_dealStatus[dealId] == 0, "Deal not pending");
        _dealStatus[dealId] = 3; // disputed
    }

    function resolveDispute(uint256 dealId, bool releaseToSeller) external {
        require(msg.sender == _arbiter, "Not arbiter");
        require(_dealStatus[dealId] == 3, "Not disputed");

        if (releaseToSeller) {
            uint256 amount = _dealAmount[dealId];
            uint256 fee = (amount * _feeRate) / 10000;
            _feesCollected += fee;
            _dealStatus[dealId] = 1; // released
        } else {
            _dealStatus[dealId] = 2; // refunded
        }
    }
}
