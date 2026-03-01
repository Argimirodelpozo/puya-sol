// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simple staking contract where users stake tokens and earn rewards.
 * Rewards are calculated as: staked * rewardRate * timeElapsed / 1000000.
 */
contract StakingTest {
    address private _owner;
    uint256 private _rewardRate;
    uint256 private _totalStaked;

    mapping(address => uint256) private _staked;
    mapping(address => uint256) private _stakeTime;
    mapping(address => uint256) private _rewards;

    constructor() {
        _owner = msg.sender;
        _rewardRate = 100; // 100 per million per time unit
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function rewardRate() external view returns (uint256) {
        return _rewardRate;
    }

    function totalStaked() external view returns (uint256) {
        return _totalStaked;
    }

    function stakedOf(address account) external view returns (uint256) {
        return _staked[account];
    }

    function stakeTimeOf(address account) external view returns (uint256) {
        return _stakeTime[account];
    }

    function rewardsOf(address account) external view returns (uint256) {
        return _rewards[account];
    }

    function setRewardRate(uint256 rate) external {
        require(msg.sender == _owner, "Not owner");
        _rewardRate = rate;
    }

    function stake(address account, uint256 amount, uint256 currentTime) external {
        require(amount > 0, "Cannot stake 0");

        // Calculate and store any pending rewards first
        if (_staked[account] > 0) {
            uint256 elapsed = currentTime - _stakeTime[account];
            uint256 reward = (_staked[account] * _rewardRate * elapsed) / 1000000;
            _rewards[account] += reward;
        }

        _staked[account] += amount;
        _stakeTime[account] = currentTime;
        _totalStaked += amount;
    }

    function unstake(address account, uint256 amount, uint256 currentTime) external returns (uint256) {
        require(_staked[account] >= amount, "Insufficient stake");

        // Calculate pending rewards
        uint256 elapsed = currentTime - _stakeTime[account];
        uint256 reward = (_staked[account] * _rewardRate * elapsed) / 1000000;
        _rewards[account] += reward;

        _staked[account] -= amount;
        _stakeTime[account] = currentTime;
        _totalStaked -= amount;

        return amount;
    }

    function claimRewards(address account, uint256 currentTime) external returns (uint256) {
        // Calculate pending rewards
        if (_staked[account] > 0) {
            uint256 elapsed = currentTime - _stakeTime[account];
            uint256 reward = (_staked[account] * _rewardRate * elapsed) / 1000000;
            _rewards[account] += reward;
            _stakeTime[account] = currentTime;
        }

        uint256 totalReward = _rewards[account];
        _rewards[account] = 0;
        return totalReward;
    }

    function pendingRewards(address account, uint256 currentTime) external view returns (uint256) {
        uint256 pending = _rewards[account];
        if (_staked[account] > 0) {
            uint256 elapsed = currentTime - _stakeTime[account];
            pending += (_staked[account] * _rewardRate * elapsed) / 1000000;
        }
        return pending;
    }
}
