// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract SupplyChain {
    address private _admin;
    uint256 private _shipmentCount;
    uint256 private _deliveredCount;
    uint256 private _returnedCount;

    mapping(uint256 => uint256) internal _shipmentOrigin;
    mapping(uint256 => uint256) internal _shipmentDestination;
    mapping(uint256 => uint256) internal _shipmentStatus;
    mapping(uint256 => uint256) internal _shipmentTimestamp;

    constructor() {
        _admin = msg.sender;
        _shipmentCount = 0;
        _deliveredCount = 0;
        _returnedCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getShipmentCount() external view returns (uint256) {
        return _shipmentCount;
    }

    function getDeliveredCount() external view returns (uint256) {
        return _deliveredCount;
    }

    function getReturnedCount() external view returns (uint256) {
        return _returnedCount;
    }

    function getShipmentOrigin(uint256 shipmentId) external view returns (uint256) {
        return _shipmentOrigin[shipmentId];
    }

    function getShipmentDestination(uint256 shipmentId) external view returns (uint256) {
        return _shipmentDestination[shipmentId];
    }

    function getShipmentStatus(uint256 shipmentId) external view returns (uint256) {
        return _shipmentStatus[shipmentId];
    }

    function getShipmentTimestamp(uint256 shipmentId) external view returns (uint256) {
        return _shipmentTimestamp[shipmentId];
    }

    function isDelivered(uint256 shipmentId) external view returns (bool) {
        return _shipmentStatus[shipmentId] == 2;
    }

    function isReturned(uint256 shipmentId) external view returns (bool) {
        return _shipmentStatus[shipmentId] == 3;
    }

    function isInTransit(uint256 shipmentId) external view returns (bool) {
        return _shipmentStatus[shipmentId] == 1;
    }

    function createShipment(uint256 origin, uint256 destination, uint256 timestamp) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        uint256 shipmentId = _shipmentCount;
        _shipmentOrigin[shipmentId] = origin;
        _shipmentDestination[shipmentId] = destination;
        _shipmentStatus[shipmentId] = 0;
        _shipmentTimestamp[shipmentId] = timestamp;
        _shipmentCount = shipmentId + 1;
        return shipmentId;
    }

    function startTransit(uint256 shipmentId) external {
        require(msg.sender == _admin, "Only admin");
        require(_shipmentStatus[shipmentId] == 0, "Not in created status");
        _shipmentStatus[shipmentId] = 1;
    }

    function deliver(uint256 shipmentId) external {
        require(msg.sender == _admin, "Only admin");
        require(_shipmentStatus[shipmentId] == 1, "Not in transit");
        _shipmentStatus[shipmentId] = 2;
        _deliveredCount = _deliveredCount + 1;
    }

    function returnShipment(uint256 shipmentId) external {
        require(msg.sender == _admin, "Only admin");
        require(_shipmentStatus[shipmentId] == 1, "Not in transit");
        _shipmentStatus[shipmentId] = 3;
        _returnedCount = _returnedCount + 1;
    }
}

contract SupplyChainTest is SupplyChain {
    constructor() SupplyChain() {}

    function initShipment(uint256 shipmentId) external {
        _shipmentOrigin[shipmentId] = 0;
        _shipmentDestination[shipmentId] = 0;
        _shipmentStatus[shipmentId] = 0;
        _shipmentTimestamp[shipmentId] = 0;
    }
}
