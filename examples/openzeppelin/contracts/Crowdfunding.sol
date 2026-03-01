// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Crowdfunding contract with goal, deadline, and refund support.
 */
contract CrowdfundingTest {
    address private _creator;
    uint256 private _goal;
    uint256 private _deadline;
    uint256 private _totalPledged;
    bool private _claimed;
    uint256 private _backerCount;

    mapping(address => uint256) private _pledges;

    constructor() {
        _creator = msg.sender;
        _goal = 10000;
        _deadline = 999999; // far future for testing
    }

    function creator() external view returns (address) {
        return _creator;
    }

    function goal() external view returns (uint256) {
        return _goal;
    }

    function deadline() external view returns (uint256) {
        return _deadline;
    }

    function totalPledged() external view returns (uint256) {
        return _totalPledged;
    }

    function claimed() external view returns (bool) {
        return _claimed;
    }

    function backerCount() external view returns (uint256) {
        return _backerCount;
    }

    function pledgeOf(address backer) external view returns (uint256) {
        return _pledges[backer];
    }

    function isGoalReached() external view returns (bool) {
        return _totalPledged >= _goal;
    }

    function pledge(address backer, uint256 amount, uint256 currentTime) external {
        require(currentTime < _deadline, "Campaign ended");
        require(amount > 0, "Amount must be > 0");
        require(!_claimed, "Already claimed");

        if (_pledges[backer] == 0) {
            _backerCount += 1;
        }

        _pledges[backer] += amount;
        _totalPledged += amount;
    }

    function unpledge(address backer, uint256 amount) external {
        require(_pledges[backer] >= amount, "Insufficient pledge");
        require(!_claimed, "Already claimed");

        _pledges[backer] -= amount;
        _totalPledged -= amount;

        if (_pledges[backer] == 0) {
            _backerCount -= 1;
        }
    }

    function claim() external {
        require(msg.sender == _creator, "Not creator");
        require(_totalPledged >= _goal, "Goal not reached");
        require(!_claimed, "Already claimed");

        _claimed = true;
    }

    function refund(address backer) external returns (uint256) {
        require(!_claimed, "Already claimed");

        uint256 amount = _pledges[backer];
        if (amount > 0) {
            _pledges[backer] = 0;
            _totalPledged -= amount;
            _backerCount -= 1;
        }
        return amount;
    }
}
