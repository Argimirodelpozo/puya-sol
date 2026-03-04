// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/**
 * @title TokenVesting
 * @notice Manages multiple vesting schedules with cliff and linear vesting.
 * Exercises: nested flat mappings, arithmetic with block numbers,
 * min/max calculations, multi-beneficiary management.
 *
 * Each schedule has: beneficiary, totalAmount, startBlock, cliffDuration,
 * vestingDuration, releasedAmount.
 */
contract TokenVesting {
    address public admin;
    uint256 public scheduleCount;

    // Schedule fields (flat mappings)
    mapping(uint256 => address) private _scheduleBeneficiary;
    mapping(uint256 => uint256) private _scheduleTotalAmount;
    mapping(uint256 => uint256) private _scheduleStartBlock;
    mapping(uint256 => uint256) private _scheduleCliffDuration;
    mapping(uint256 => uint256) private _scheduleVestingDuration;
    mapping(uint256 => uint256) private _scheduleReleasedAmount;
    mapping(uint256 => bool) private _scheduleRevoked;

    event ScheduleCreated(uint256 indexed scheduleId, address indexed beneficiary, uint256 totalAmount);
    event TokensReleased(uint256 indexed scheduleId, uint256 amount);
    event ScheduleRevoked(uint256 indexed scheduleId);

    constructor() {
        admin = msg.sender;
    }

    function createSchedule(
        address beneficiary,
        uint256 totalAmount,
        uint256 cliffDuration,
        uint256 vestingDuration
    ) external returns (uint256) {
        require(msg.sender == admin, "not admin");
        require(vestingDuration > 0, "duration must be > 0");
        require(totalAmount > 0, "amount must be > 0");

        scheduleCount = scheduleCount + 1;
        uint256 id = scheduleCount;

        _scheduleBeneficiary[id] = beneficiary;
        _scheduleTotalAmount[id] = totalAmount;
        _scheduleStartBlock[id] = 100; // Use a fixed start for testability
        _scheduleCliffDuration[id] = cliffDuration;
        _scheduleVestingDuration[id] = vestingDuration;
        _scheduleReleasedAmount[id] = 0;
        _scheduleRevoked[id] = false;

        return id;
    }

    /**
     * @notice Calculate vested amount at a given block number.
     */
    function vestedAmount(uint256 scheduleId, uint256 currentBlock) external view returns (uint256) {
        return _vestedAmount(scheduleId, currentBlock);
    }

    function _vestedAmount(uint256 scheduleId, uint256 currentBlock) internal view returns (uint256) {
        uint256 startBlock = _scheduleStartBlock[scheduleId];
        uint256 cliff = _scheduleCliffDuration[scheduleId];
        uint256 duration = _scheduleVestingDuration[scheduleId];
        uint256 total = _scheduleTotalAmount[scheduleId];

        // Before cliff: nothing vested
        if (currentBlock < startBlock + cliff) {
            return 0;
        }

        // After full vesting: everything vested
        if (currentBlock >= startBlock + duration) {
            return total;
        }

        // Linear vesting between cliff and end
        uint256 elapsed = currentBlock - startBlock;
        return (total * elapsed) / duration;
    }

    /**
     * @notice Calculate releasable amount (vested - already released).
     */
    function releasableAmount(uint256 scheduleId, uint256 currentBlock) external view returns (uint256) {
        uint256 vested = _vestedAmount(scheduleId, currentBlock);
        uint256 released = _scheduleReleasedAmount[scheduleId];
        if (vested > released) {
            return vested - released;
        }
        return 0;
    }

    /**
     * @notice Release vested tokens to beneficiary.
     */
    function release(uint256 scheduleId, uint256 currentBlock) external returns (uint256) {
        require(!_scheduleRevoked[scheduleId], "schedule revoked");
        address beneficiary = _scheduleBeneficiary[scheduleId];
        require(msg.sender == beneficiary || msg.sender == admin, "not authorized");

        uint256 vested = _vestedAmount(scheduleId, currentBlock);
        uint256 released = _scheduleReleasedAmount[scheduleId];
        require(vested > released, "nothing to release");

        uint256 amount = vested - released;
        _scheduleReleasedAmount[scheduleId] = vested;

        // In a real contract, this would transfer tokens
        return amount;
    }

    /**
     * @notice Revoke a vesting schedule (admin only).
     */
    function revoke(uint256 scheduleId) external {
        require(msg.sender == admin, "not admin");
        require(!_scheduleRevoked[scheduleId], "already revoked");
        _scheduleRevoked[scheduleId] = true;
    }

    // ─── View Functions ───

    function getScheduleCount() external view returns (uint256) {
        return scheduleCount;
    }

    function getBeneficiary(uint256 scheduleId) external view returns (address) {
        return _scheduleBeneficiary[scheduleId];
    }

    function getTotalAmount(uint256 scheduleId) external view returns (uint256) {
        return _scheduleTotalAmount[scheduleId];
    }

    function getReleasedAmount(uint256 scheduleId) external view returns (uint256) {
        return _scheduleReleasedAmount[scheduleId];
    }

    function isRevoked(uint256 scheduleId) external view returns (bool) {
        return _scheduleRevoked[scheduleId];
    }

    function getStartBlock(uint256 scheduleId) external view returns (uint256) {
        return _scheduleStartBlock[scheduleId];
    }

    function getCliffDuration(uint256 scheduleId) external view returns (uint256) {
        return _scheduleCliffDuration[scheduleId];
    }

    function getVestingDuration(uint256 scheduleId) external view returns (uint256) {
        return _scheduleVestingDuration[scheduleId];
    }
}
