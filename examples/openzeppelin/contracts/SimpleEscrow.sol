// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract SimpleEscrow {
    address private admin;
    uint256 private totalDeposits;
    uint256 private totalWithdrawn;
    uint256 private depositorCount;
    bool private frozen;

    mapping(address => uint256) internal _deposits;
    mapping(address => uint256) internal _withdrawn;
    mapping(address => uint256) internal _depositorIndex;

    constructor() {
        admin = msg.sender;
    }

    function deposit(address depositor, uint256 amount) public {
        require(!frozen, "Escrow is frozen");
        _deposits[depositor] += amount;
        totalDeposits += amount;
        if (_depositorIndex[depositor] == 0) {
            depositorCount += 1;
            _depositorIndex[depositor] = depositorCount;
        }
    }

    function withdraw(address depositor, uint256 amount) public returns (uint256) {
        require(!frozen, "Escrow is frozen");
        uint256 avail = _deposits[depositor] - _withdrawn[depositor];
        require(avail >= amount, "Insufficient available");
        _withdrawn[depositor] += amount;
        totalWithdrawn += amount;
        return amount;
    }

    function available(address depositor) public view returns (uint256) {
        return _deposits[depositor] - _withdrawn[depositor];
    }

    function getDeposits(address depositor) public view returns (uint256) {
        return _deposits[depositor];
    }

    function getWithdrawn(address depositor) public view returns (uint256) {
        return _withdrawn[depositor];
    }

    function isDepositor(address depositor) public view returns (bool) {
        return _depositorIndex[depositor] != 0;
    }

    function getTotalDeposits() public view returns (uint256) {
        return totalDeposits;
    }

    function getTotalWithdrawn() public view returns (uint256) {
        return totalWithdrawn;
    }

    function getDepositorCount() public view returns (uint256) {
        return depositorCount;
    }

    function getAdmin() public view returns (address) {
        return admin;
    }

    function emergencyFreeze() public {
        require(msg.sender == admin, "Only admin");
        frozen = true;
    }

    function isFrozen() public view returns (bool) {
        return frozen;
    }
}

contract SimpleEscrowTest is SimpleEscrow {
    constructor() SimpleEscrow() {}

    function initDepositor(address addr) public {
        _deposits[addr] = 0;
        _withdrawn[addr] = 0;
        _depositorIndex[addr] = 0;
    }
}
