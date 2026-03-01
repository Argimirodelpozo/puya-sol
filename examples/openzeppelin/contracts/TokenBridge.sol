// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract TokenBridge {
    address public admin;
    uint256 public bridgeFee;
    uint256 public transferCount;
    uint256 public totalBridged;
    uint256 public totalFees;
    bool public paused;

    mapping(uint256 => address) private _transferSender;
    mapping(uint256 => uint256) private _transferAmount;
    mapping(uint256 => uint256) private _transferDestChain;
    mapping(uint256 => bool) private _transferCompleted;
    mapping(uint256 => bool) private _transferRefunded;

    constructor(uint256 bridgeFee_) {
        admin = msg.sender;
        bridgeFee = bridgeFee_;
    }

    function initiate(address sender, uint256 amount, uint256 destChain) public returns (uint256) {
        require(!paused, "Bridge is paused");
        transferCount = transferCount + 1;
        uint256 transferId = transferCount;
        uint256 fee = amount * bridgeFee / 10000;
        uint256 netAmount = amount - fee;
        _transferSender[transferId] = sender;
        _transferAmount[transferId] = netAmount;
        _transferDestChain[transferId] = destChain;
        totalBridged = totalBridged + netAmount;
        totalFees = totalFees + fee;
        return transferId;
    }

    function complete(uint256 transferId) public {
        require(msg.sender == admin, "Only admin");
        require(!_transferCompleted[transferId], "Already completed");
        require(!_transferRefunded[transferId], "Already refunded");
        _transferCompleted[transferId] = true;
    }

    function refund(uint256 transferId) public {
        require(msg.sender == admin, "Only admin");
        require(!_transferCompleted[transferId], "Already completed");
        require(!_transferRefunded[transferId], "Already refunded");
        _transferRefunded[transferId] = true;
    }

    function pause() public {
        require(msg.sender == admin, "Only admin");
        paused = true;
    }

    function unpause() public {
        require(msg.sender == admin, "Only admin");
        paused = false;
    }

    function isPaused() public view returns (bool) {
        return paused;
    }

    function getTransferSender(uint256 id) public view returns (address) {
        return _transferSender[id];
    }

    function getTransferAmount(uint256 id) public view returns (uint256) {
        return _transferAmount[id];
    }

    function getTransferDestChain(uint256 id) public view returns (uint256) {
        return _transferDestChain[id];
    }

    function isTransferCompleted(uint256 id) public view returns (bool) {
        return _transferCompleted[id];
    }

    function isTransferRefunded(uint256 id) public view returns (bool) {
        return _transferRefunded[id];
    }

    function getTotalBridged() public view returns (uint256) {
        return totalBridged;
    }

    function getTotalFees() public view returns (uint256) {
        return totalFees;
    }

    function getTransferCount() public view returns (uint256) {
        return transferCount;
    }

    function getAdmin() public view returns (address) {
        return admin;
    }

    function setBridgeFee(uint256 newFee) public {
        require(msg.sender == admin, "Only admin");
        bridgeFee = newFee;
    }
}

contract TokenBridgeTest is TokenBridge {
    constructor() TokenBridge(100) {}
}
