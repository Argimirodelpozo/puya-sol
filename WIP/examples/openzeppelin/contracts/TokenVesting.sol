// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Token vesting contract that releases tokens over time.
 * Uses a linear vesting schedule per beneficiary.
 */
contract TokenVestingTest {
    struct VestingSchedule {
        uint256 totalAmount;
        uint256 released;
        uint256 startTime;
        uint256 duration;
    }

    address private _owner;
    mapping(address => uint256) private _vestingTotal;
    mapping(address => uint256) private _vestingReleased;
    mapping(address => uint256) private _vestingStart;
    mapping(address => uint256) private _vestingDuration;
    uint256 private _totalAllocated;

    constructor() {
        _owner = msg.sender;
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function totalAllocated() external view returns (uint256) {
        return _totalAllocated;
    }

    function createVesting(
        address beneficiary,
        uint256 totalAmount,
        uint256 startTime,
        uint256 duration
    ) external {
        require(msg.sender == _owner, "Not owner");
        require(totalAmount > 0, "Amount must be > 0");
        require(duration > 0, "Duration must be > 0");
        require(_vestingTotal[beneficiary] == 0, "Already has vesting");

        _vestingTotal[beneficiary] = totalAmount;
        _vestingReleased[beneficiary] = 0;
        _vestingStart[beneficiary] = startTime;
        _vestingDuration[beneficiary] = duration;
        _totalAllocated += totalAmount;
    }

    function vestingTotal(address beneficiary) external view returns (uint256) {
        return _vestingTotal[beneficiary];
    }

    function vestingReleased(address beneficiary) external view returns (uint256) {
        return _vestingReleased[beneficiary];
    }

    function vestingStart(address beneficiary) external view returns (uint256) {
        return _vestingStart[beneficiary];
    }

    function vestingDuration(address beneficiary) external view returns (uint256) {
        return _vestingDuration[beneficiary];
    }

    function vestedAmount(address beneficiary, uint256 currentTime) external view returns (uint256) {
        uint256 total = _vestingTotal[beneficiary];
        if (total == 0) return 0;

        uint256 start = _vestingStart[beneficiary];
        uint256 dur = _vestingDuration[beneficiary];

        if (currentTime < start) return 0;
        if (currentTime >= start + dur) return total;

        return (total * (currentTime - start)) / dur;
    }

    function releasable(address beneficiary, uint256 currentTime) external view returns (uint256) {
        uint256 total = _vestingTotal[beneficiary];
        if (total == 0) return 0;

        uint256 start = _vestingStart[beneficiary];
        uint256 dur = _vestingDuration[beneficiary];
        uint256 released = _vestingReleased[beneficiary];

        uint256 vested;
        if (currentTime < start) {
            vested = 0;
        } else if (currentTime >= start + dur) {
            vested = total;
        } else {
            vested = (total * (currentTime - start)) / dur;
        }

        if (vested <= released) return 0;
        return vested - released;
    }

    function release(address beneficiary, uint256 currentTime) external returns (uint256) {
        require(msg.sender == _owner, "Not owner");
        uint256 total = _vestingTotal[beneficiary];
        require(total > 0, "No vesting");

        uint256 start = _vestingStart[beneficiary];
        uint256 dur = _vestingDuration[beneficiary];
        uint256 released = _vestingReleased[beneficiary];

        uint256 vested;
        if (currentTime < start) {
            vested = 0;
        } else if (currentTime >= start + dur) {
            vested = total;
        } else {
            vested = (total * (currentTime - start)) / dur;
        }

        uint256 amount = 0;
        if (vested > released) {
            amount = vested - released;
            _vestingReleased[beneficiary] = released + amount;
        }
        return amount;
    }

    function revokeVesting(address beneficiary) external {
        require(msg.sender == _owner, "Not owner");
        uint256 total = _vestingTotal[beneficiary];
        uint256 released = _vestingReleased[beneficiary];

        _totalAllocated -= (total - released);
        _vestingTotal[beneficiary] = 0;
        _vestingReleased[beneficiary] = 0;
        _vestingStart[beneficiary] = 0;
        _vestingDuration[beneficiary] = 0;
    }
}
