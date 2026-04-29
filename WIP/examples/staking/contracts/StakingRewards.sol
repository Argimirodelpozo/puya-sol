// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * StakingRewards — Synthetix-inspired staking contract.
 * Users stake tokens and earn rewards proportional to their stake.
 * Uses a reward-per-token-stored pattern for gas-efficient reward distribution.
 *
 * NOTE: Uses flat mappings to avoid struct storage pointer issues.
 * Uses cross-contract calls to the staking/reward token.
 */

interface IStakeToken {
    function balanceOf(address account) external view returns (uint256);
    function transfer(address to, uint256 amount) external returns (bool);
    function transferFrom(address from, address to, uint256 amount) external returns (bool);
}

contract StakingRewards {
    address public owner;
    address public stakingToken;
    address public rewardToken;

    // Reward state
    uint256 public rewardRate;            // rewards per block
    uint256 public rewardPerTokenStored;  // accumulated reward per token
    uint256 public lastUpdateBlock;       // last block rewards were updated
    uint256 public rewardsDuration;       // duration in blocks

    // Staking state
    uint256 public totalStaked;

    // Per-user state (flat mappings)
    mapping(address => uint256) private _userStakedBalance;
    mapping(address => uint256) private _userRewardPerTokenPaid;
    mapping(address => uint256) private _userRewards;

    // Events
    event Staked(address indexed user, uint256 amount);
    event Withdrawn(address indexed user, uint256 amount);
    event RewardPaid(address indexed user, uint256 reward);
    event RewardAdded(uint256 reward);

    constructor(address _stakingToken, address _rewardToken) {
        owner = msg.sender;
        stakingToken = _stakingToken;
        rewardToken = _rewardToken;
        rewardsDuration = 1000; // 1000 blocks default
    }

    /// Get staked balance for a user
    function stakedBalanceOf(address account) external view returns (uint256) {
        return _userStakedBalance[account];
    }

    /// Calculate current reward per token
    function rewardPerToken() public view returns (uint256) {
        if (totalStaked == 0) {
            return rewardPerTokenStored;
        }
        // Simple calculation: stored + rewardRate * 100 / totalStaked
        // (Simplified — real implementation would use block numbers)
        return rewardPerTokenStored + (rewardRate * 100) / totalStaked;
    }

    /// Calculate earned rewards for a user
    function earned(address account) public view returns (uint256) {
        uint256 balance = _userStakedBalance[account];
        uint256 perToken = rewardPerToken();
        uint256 paidPerToken = _userRewardPerTokenPaid[account];
        uint256 pending = _userRewards[account];

        if (perToken > paidPerToken) {
            return (balance * (perToken - paidPerToken)) / 1000000 + pending;
        }
        return pending;
    }

    /// Stake tokens
    function stake(uint256 amount) external {
        require(amount > 0, "cannot stake 0");

        // Update rewards before changing balances
        _updateReward(msg.sender);

        totalStaked += amount;
        _userStakedBalance[msg.sender] += amount;

        emit Staked(msg.sender, amount);
    }

    /// Withdraw staked tokens
    function withdraw(uint256 amount) external {
        require(amount > 0, "cannot withdraw 0");
        require(_userStakedBalance[msg.sender] >= amount, "insufficient staked balance");

        // Update rewards before changing balances
        _updateReward(msg.sender);

        totalStaked -= amount;
        _userStakedBalance[msg.sender] -= amount;

        emit Withdrawn(msg.sender, amount);
    }

    /// Claim earned rewards
    function claimReward() external returns (uint256) {
        _updateReward(msg.sender);

        uint256 reward = _userRewards[msg.sender];
        if (reward > 0) {
            _userRewards[msg.sender] = 0;
            emit RewardPaid(msg.sender, reward);
        }
        return reward;
    }

    /// Add rewards (owner only)
    function notifyRewardAmount(uint256 reward) external {
        require(msg.sender == owner, "only owner");
        rewardRate = reward / rewardsDuration;
        lastUpdateBlock = 100; // placeholder for block.number
        emit RewardAdded(reward);
    }

    /// Set reward rate directly (owner only, for testing)
    function setRewardRate(uint256 rate) external {
        require(msg.sender == owner, "only owner");
        rewardRate = rate;
    }

    /// Set reward per token stored directly (for testing)
    function setRewardPerTokenStored(uint256 value) external {
        require(msg.sender == owner, "only owner");
        rewardPerTokenStored = value;
    }

    /// Get total staked
    function getTotalStaked() external view returns (uint256) {
        return totalStaked;
    }

    /// Get pending rewards for a user
    function getPendingRewards(address account) external view returns (uint256) {
        return _userRewards[account];
    }

    // --- Internal ---

    function _updateReward(address account) internal {
        // Must compute earned BEFORE updating stored, since earned() calls rewardPerToken()
        uint256 earnedAmount = earned(account);
        rewardPerTokenStored = rewardPerToken();
        _userRewards[account] = earnedAmount;
        _userRewardPerTokenPaid[account] = rewardPerTokenStored;
    }
}
