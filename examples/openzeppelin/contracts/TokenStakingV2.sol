// SPDX-License-Identifier: MIT
// Token staking with reward calculation and lockup periods
// Tests: multiplication/division with biguint, time-based logic, complex state updates

pragma solidity ^0.8.20;

error StakingZeroAmount();
error StakingInsufficientStake(uint256 available, uint256 requested);
error StakingLocked(uint256 unlockTime, uint256 currentTime);
error StakingNoRewards();
error StakingNotOwner(address caller);

contract TokenStakingV2 {
    event Staked(address indexed staker, uint256 amount, uint256 lockUntil);
    event Unstaked(address indexed staker, uint256 amount);
    event RewardClaimed(address indexed staker, uint256 reward);
    event RewardRateUpdated(uint256 oldRate, uint256 newRate);

    address private _owner;

    // Staking state per user
    mapping(address => uint256) private _stakedBalances;
    mapping(address => uint256) private _lockUntil;
    mapping(address => uint256) private _lastRewardTime;
    mapping(address => uint256) private _pendingRewards;

    // Global state
    uint256 private _totalStaked;
    uint256 private _rewardRatePerSecond; // reward tokens per second per 1000 staked
    uint256 private _totalRewardsPaid;
    uint256 private _stakerCount;
    uint256 private _lockDuration; // in seconds

    uint256 private constant REWARD_PRECISION = 1000;

    constructor() {
        _owner = msg.sender;
        _rewardRatePerSecond = 1; // 0.1% per second per 1000 staked
        _lockDuration = 100; // 100 seconds lock
    }

    modifier onlyOwner() {
        if (msg.sender != _owner) {
            revert StakingNotOwner(msg.sender);
        }
        _;
    }

    function stake(uint256 amount) public {
        if (amount == 0) {
            revert StakingZeroAmount();
        }

        // Update pending rewards before changing balance
        _updateRewards(msg.sender);

        if (_stakedBalances[msg.sender] == 0) {
            _stakerCount += 1;
        }

        _stakedBalances[msg.sender] += amount;
        _totalStaked += amount;
        _lockUntil[msg.sender] = block.timestamp + _lockDuration;

        emit Staked(msg.sender, amount, block.timestamp + _lockDuration);
    }

    function unstake(uint256 amount) public {
        if (amount == 0) {
            revert StakingZeroAmount();
        }

        uint256 staked = _stakedBalances[msg.sender];
        if (staked < amount) {
            revert StakingInsufficientStake(staked, amount);
        }

        uint256 unlock = _lockUntil[msg.sender];
        if (block.timestamp < unlock) {
            revert StakingLocked(unlock, block.timestamp);
        }

        // Update pending rewards before changing balance
        _updateRewards(msg.sender);

        _stakedBalances[msg.sender] = staked - amount;
        _totalStaked -= amount;

        if (_stakedBalances[msg.sender] == 0) {
            _stakerCount -= 1;
        }

        emit Unstaked(msg.sender, amount);
    }

    function claimRewards() public {
        _updateRewards(msg.sender);

        uint256 rewards = _pendingRewards[msg.sender];
        if (rewards == 0) {
            revert StakingNoRewards();
        }

        _pendingRewards[msg.sender] = 0;
        _totalRewardsPaid += rewards;

        emit RewardClaimed(msg.sender, rewards);
    }

    function setRewardRate(uint256 newRate) public onlyOwner {
        uint256 oldRate = _rewardRatePerSecond;
        _rewardRatePerSecond = newRate;
        emit RewardRateUpdated(oldRate, newRate);
    }

    function setLockDuration(uint256 duration) public onlyOwner {
        _lockDuration = duration;
    }

    // --- View functions ---

    function stakedBalance(address staker) public view returns (uint256) {
        return _stakedBalances[staker];
    }

    function lockUntil(address staker) public view returns (uint256) {
        return _lockUntil[staker];
    }

    function pendingRewards(address staker) public view returns (uint256) {
        uint256 pending = _pendingRewards[staker];
        uint256 staked = _stakedBalances[staker];
        if (staked > 0 && _lastRewardTime[staker] > 0) {
            uint256 elapsed = block.timestamp - _lastRewardTime[staker];
            uint256 newRewards = (staked * _rewardRatePerSecond * elapsed) / REWARD_PRECISION;
            pending += newRewards;
        }
        return pending;
    }

    function totalStaked() public view returns (uint256) {
        return _totalStaked;
    }

    function totalRewardsPaid() public view returns (uint256) {
        return _totalRewardsPaid;
    }

    function rewardRate() public view returns (uint256) {
        return _rewardRatePerSecond;
    }

    function lockDuration() public view returns (uint256) {
        return _lockDuration;
    }

    function stakerCount() public view returns (uint256) {
        return _stakerCount;
    }

    function owner() public view returns (address) {
        return _owner;
    }

    function isLocked(address staker) public view returns (bool) {
        return block.timestamp < _lockUntil[staker];
    }

    // --- Internal ---

    function _updateRewards(address staker) private {
        uint256 staked = _stakedBalances[staker];
        if (staked > 0 && _lastRewardTime[staker] > 0) {
            uint256 elapsed = block.timestamp - _lastRewardTime[staker];
            uint256 newRewards = (staked * _rewardRatePerSecond * elapsed) / REWARD_PRECISION;
            _pendingRewards[staker] += newRewards;
        }
        _lastRewardTime[staker] = block.timestamp;
    }
}
