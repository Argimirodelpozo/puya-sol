// SPDX-License-Identifier: MIT
// Inspired by OpenZeppelin VestingWallet.sol
// Demonstrates: time-based math, multiple mappings, view functions, division
pragma solidity ^0.8.20;

contract LinearVestingTest {
    mapping(address => uint256) private _totalAllocation;
    mapping(address => uint256) private _released;
    mapping(address => uint256) private _startTime;
    mapping(address => uint256) private _duration;

    event VestingCreated(address indexed beneficiary, uint256 amount, uint256 start, uint256 duration);
    event TokensReleased(address indexed beneficiary, uint256 amount);

    error NoAllocation();
    error NothingToRelease();
    error ZeroDuration();

    function createVesting(address beneficiary, uint256 amount, uint256 start, uint256 dur) external {
        if (dur == 0) revert ZeroDuration();
        _totalAllocation[beneficiary] = amount;
        _startTime[beneficiary] = start;
        _duration[beneficiary] = dur;
        _released[beneficiary] = 0;
        emit VestingCreated(beneficiary, amount, start, dur);
    }

    function vestedAmount(address beneficiary, uint256 currentTime) public view returns (uint256) {
        uint256 total = _totalAllocation[beneficiary];
        if (total == 0) return 0;
        uint256 start = _startTime[beneficiary];
        uint256 dur = _duration[beneficiary];
        if (currentTime < start) return 0;
        if (currentTime >= start + dur) return total;
        return (total * (currentTime - start)) / dur;
    }

    function releasableAmount(address beneficiary, uint256 currentTime) external view returns (uint256) {
        uint256 vested = vestedAmount(beneficiary, currentTime);
        uint256 alreadyReleased = _released[beneficiary];
        if (vested <= alreadyReleased) return 0;
        return vested - alreadyReleased;
    }

    function release(address beneficiary, uint256 currentTime) external {
        uint256 vested = vestedAmount(beneficiary, currentTime);
        uint256 alreadyReleased = _released[beneficiary];
        if (vested <= alreadyReleased) revert NothingToRelease();
        uint256 amount = vested - alreadyReleased;
        _released[beneficiary] += amount;
        emit TokensReleased(beneficiary, amount);
    }

    function allocation(address beneficiary) external view returns (uint256) {
        return _totalAllocation[beneficiary];
    }

    function released(address beneficiary) external view returns (uint256) {
        return _released[beneficiary];
    }

    function startTime(address beneficiary) external view returns (uint256) {
        return _startTime[beneficiary];
    }

    function duration(address beneficiary) external view returns (uint256) {
        return _duration[beneficiary];
    }
}
