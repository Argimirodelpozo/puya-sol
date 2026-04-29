// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Crowdfunding pledge collection system.
 * Admin creates pledges with a goal amount, users contribute,
 * and admin marks pledges as funded. Tracks totals.
 */
abstract contract PledgePool {
    address private _admin;
    uint256 private _pledgeCount;
    uint256 private _totalPledged;

    mapping(uint256 => uint256) internal _pledgeAmount;
    mapping(uint256 => uint256) internal _pledgeGoal;
    mapping(uint256 => bool) internal _pledgeFunded;

    constructor() {
        _admin = msg.sender;
        _pledgeCount = 0;
        _totalPledged = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getPledgeCount() external view returns (uint256) {
        return _pledgeCount;
    }

    function getTotalPledged() external view returns (uint256) {
        return _totalPledged;
    }

    function getPledgeAmount(uint256 pledgeId) external view returns (uint256) {
        return _pledgeAmount[pledgeId];
    }

    function getPledgeGoal(uint256 pledgeId) external view returns (uint256) {
        return _pledgeGoal[pledgeId];
    }

    function isPledgeFunded(uint256 pledgeId) external view returns (bool) {
        return _pledgeFunded[pledgeId];
    }

    function createPledge(uint256 goal) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        require(goal > 0, "Goal must be positive");
        uint256 id = _pledgeCount;
        _pledgeAmount[id] = 0;
        _pledgeGoal[id] = goal;
        _pledgeFunded[id] = false;
        _pledgeCount = id + 1;
        return id;
    }

    function contribute(uint256 pledgeId, uint256 amount) external {
        require(pledgeId < _pledgeCount, "Pledge does not exist");
        require(!_pledgeFunded[pledgeId], "Pledge already funded");
        require(amount > 0, "Amount must be positive");
        _pledgeAmount[pledgeId] = _pledgeAmount[pledgeId] + amount;
        _totalPledged = _totalPledged + amount;
    }

    function markFunded(uint256 pledgeId) external {
        require(msg.sender == _admin, "Only admin");
        require(pledgeId < _pledgeCount, "Pledge does not exist");
        require(!_pledgeFunded[pledgeId], "Pledge already funded");
        require(_pledgeAmount[pledgeId] >= _pledgeGoal[pledgeId], "Goal not reached");
        _pledgeFunded[pledgeId] = true;
    }
}

contract PledgePoolTest is PledgePool {
    constructor() PledgePool() {}

    function initPledge(uint256 pledgeId) external {
        _pledgeAmount[pledgeId] = 0;
        _pledgeGoal[pledgeId] = 0;
        _pledgeFunded[pledgeId] = false;
    }
}
