// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Escrow3Party {
    address private _admin;
    uint256 private _dealCount;
    uint256 private _totalEscrowed;
    uint256 private _totalReleased;
    uint256 private _totalRefunded;

    mapping(uint256 => address) internal _dealBuyer;
    mapping(uint256 => address) internal _dealSeller;
    mapping(uint256 => address) internal _dealArbiter;
    mapping(uint256 => uint256) internal _dealAmount;
    mapping(uint256 => uint256) internal _dealStatus;

    constructor() {
        _admin = msg.sender;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function dealCount() external view returns (uint256) {
        return _dealCount;
    }

    function totalEscrowed() external view returns (uint256) {
        return _totalEscrowed;
    }

    function totalReleased() external view returns (uint256) {
        return _totalReleased;
    }

    function totalRefunded() external view returns (uint256) {
        return _totalRefunded;
    }

    function getDealBuyer(uint256 dealId) external view returns (address) {
        return _dealBuyer[dealId];
    }

    function getDealSeller(uint256 dealId) external view returns (address) {
        return _dealSeller[dealId];
    }

    function getDealArbiter(uint256 dealId) external view returns (address) {
        return _dealArbiter[dealId];
    }

    function getDealAmount(uint256 dealId) external view returns (uint256) {
        return _dealAmount[dealId];
    }

    function getDealStatus(uint256 dealId) external view returns (uint256) {
        return _dealStatus[dealId];
    }

    function createDeal(
        address buyer,
        address seller,
        address arbiter,
        uint256 amount
    ) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        require(amount > 0, "Amount must be positive");

        uint256 dealId = _dealCount;
        _dealBuyer[dealId] = buyer;
        _dealSeller[dealId] = seller;
        _dealArbiter[dealId] = arbiter;
        _dealAmount[dealId] = amount;
        _dealStatus[dealId] = 0;

        _dealCount = dealId + 1;
        return dealId;
    }

    function fundDeal(uint256 dealId) external {
        require(dealId < _dealCount, "Deal does not exist");
        require(_dealStatus[dealId] == 0, "Deal not in created state");
        require(msg.sender == _dealBuyer[dealId], "Only buyer can fund");

        _dealStatus[dealId] = 1;
        _totalEscrowed = _totalEscrowed + _dealAmount[dealId];
    }

    function releaseDeal(uint256 dealId) external {
        require(dealId < _dealCount, "Deal does not exist");
        require(_dealStatus[dealId] == 1, "Deal not in funded state");
        require(
            msg.sender == _dealBuyer[dealId] || msg.sender == _dealArbiter[dealId],
            "Only buyer or arbiter"
        );

        _dealStatus[dealId] = 2;
        _totalReleased = _totalReleased + _dealAmount[dealId];
    }

    function refundDeal(uint256 dealId) external {
        require(dealId < _dealCount, "Deal does not exist");
        uint256 status = _dealStatus[dealId];
        require(status == 1 || status == 4, "Deal not in funded or disputed state");
        require(
            msg.sender == _dealSeller[dealId] || msg.sender == _dealArbiter[dealId],
            "Only seller or arbiter"
        );

        _dealStatus[dealId] = 3;
        _totalRefunded = _totalRefunded + _dealAmount[dealId];
    }

    function disputeDeal(uint256 dealId) external {
        require(dealId < _dealCount, "Deal does not exist");
        require(_dealStatus[dealId] == 1, "Deal not in funded state");
        require(
            msg.sender == _dealBuyer[dealId] || msg.sender == _dealSeller[dealId],
            "Only buyer or seller"
        );

        _dealStatus[dealId] = 4;
    }
}

contract Escrow3PartyTest is Escrow3Party {
    constructor() Escrow3Party() {}

    function initDeal(uint256 dealId) external {
        _dealBuyer[dealId] = address(0);
        _dealSeller[dealId] = address(0);
        _dealArbiter[dealId] = address(0);
        _dealAmount[dealId] = 0;
        _dealStatus[dealId] = 0;
    }
}
