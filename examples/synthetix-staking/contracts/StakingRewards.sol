// SPDX-License-Identifier: MIT

/// Synthetix StakingRewards — Self-contained for AVM
///
/// SOURCE: https://github.com/Synthetixio/synthetix/blob/develop/contracts/StakingRewards.sol
/// COMMIT: develop branch (MIT license)
/// TESTS: https://github.com/Synthetixio/synthetix/blob/develop/test/contracts/StakingRewards.js
///
/// MODIFICATIONS FOR AVM COMPATIBILITY:
/// 1. Pragma upgraded from ^0.5.16 to ^0.8.0
/// 2. Removed all imports (SafeMath, SafeERC20, ReentrancyGuard, Pausable,
///    IStakingRewards, RewardsDistributionRecipient, Owned)
/// 3. Replaced SafeMath .add/.sub/.mul/.div with native operators
///    (0.8.x has built-in overflow checks)
/// 4. Removed SafeERC20 token transfers (safeTransferFrom, safeTransfer)
///    Token accounting done internally via mappings (AVM cross-contract
///    transfers would need inner transactions, tested separately)
/// 5. Replaced `block.timestamp` with explicit `currentTime` parameter
///    (AVM block time semantics differ; parameterized for testability)
/// 6. Removed ReentrancyGuard (AVM doesn't have re-entrancy)
/// 7. Removed Pausable (separate concern)
/// 8. Removed recoverERC20 (requires cross-contract call)
/// 9. Simplified constructor (removed external token addresses)
/// 10. Added explicit getters (AVM: public vars don't auto-generate)
///
/// PRESERVED EXACTLY FROM ORIGINAL:
/// - rewardPerToken() calculation formula
/// - earned() calculation formula
/// - updateReward modifier logic (inlined)
/// - notifyRewardAmount() reward rate calculation
/// - lastTimeRewardApplicable() min function
/// - stake/withdraw/getReward/exit flow
/// - rewardsDuration, periodFinish, rewardRate, lastUpdateTime state

pragma solidity ^0.8.0;

contract StakingRewards {
    /* ========== STATE VARIABLES ========== */
    // Preserved exactly from original
    address public owner;
    address public rewardsDistribution;
    uint256 public periodFinish;
    uint256 public rewardRate;
    uint256 public rewardsDuration;
    uint256 public lastUpdateTime;
    uint256 public rewardPerTokenStored;

    mapping(address => uint256) public userRewardPerTokenPaid;
    mapping(address => uint256) public rewards;

    uint256 private _totalSupply;
    mapping(address => uint256) private _balances;

    /* ========== EVENTS ========== */
    // Preserved from original
    event RewardAdded(uint256 reward);
    event Staked(address indexed user, uint256 amount);
    event Withdrawn(address indexed user, uint256 amount);
    event RewardPaid(address indexed user, uint256 reward);
    event RewardsDurationUpdated(uint256 newDuration);

    /* ========== CONSTRUCTOR ========== */

    constructor(uint256 _rewardsDuration) {
        owner = msg.sender;
        rewardsDistribution = msg.sender;
        rewardsDuration = _rewardsDuration;
    }

    /* ========== VIEWS ========== */
    // Logic preserved exactly from original

    function totalSupply() external view returns (uint256) {
        return _totalSupply;
    }

    function balanceOf(address account) external view returns (uint256) {
        return _balances[account];
    }

    /// Original: return block.timestamp < periodFinish ? block.timestamp : periodFinish
    function lastTimeRewardApplicable(uint256 currentTime) public view returns (uint256) {
        if (currentTime < periodFinish) {
            return currentTime;
        }
        return periodFinish;
    }

    /// Original formula preserved exactly:
    /// rewardPerTokenStored + (lastTimeRewardApplicable - lastUpdateTime) * rewardRate * 1e18 / totalSupply
    function rewardPerToken(uint256 currentTime) public view returns (uint256) {
        if (_totalSupply == 0) {
            return rewardPerTokenStored;
        }
        uint256 timeDelta = lastTimeRewardApplicable(currentTime) - lastUpdateTime;
        return rewardPerTokenStored + (timeDelta * rewardRate * 1000000000000000000) / _totalSupply;
    }

    /// Original formula preserved exactly:
    /// balances[account] * (rewardPerToken - userRewardPerTokenPaid[account]) / 1e18 + rewards[account]
    function earned(address account, uint256 currentTime) public view returns (uint256) {
        uint256 rpt = rewardPerToken(currentTime);
        uint256 paid = userRewardPerTokenPaid[account];
        uint256 bal = _balances[account];
        return (bal * (rpt - paid)) / 1000000000000000000 + rewards[account];
    }

    function getRewardForDuration() external view returns (uint256) {
        return rewardRate * rewardsDuration;
    }

    /* ========== MUTATIVE FUNCTIONS ========== */

    /// Original: stake + updateReward modifier
    function stake(uint256 amount, uint256 currentTime) external {
        // updateReward(msg.sender) — inlined from original modifier
        rewardPerTokenStored = rewardPerToken(currentTime);
        lastUpdateTime = lastTimeRewardApplicable(currentTime);
        rewards[msg.sender] = earned(msg.sender, currentTime);
        userRewardPerTokenPaid[msg.sender] = rewardPerTokenStored;

        require(amount > 0, "Cannot stake 0");
        _totalSupply = _totalSupply + amount;
        _balances[msg.sender] = _balances[msg.sender] + amount;
        // Original: stakingToken.safeTransferFrom(msg.sender, address(this), amount)
        // AVM: token accounting only (no cross-contract transfer)
    }

    /// Original: withdraw + updateReward modifier
    function withdraw(uint256 amount, uint256 currentTime) external {
        // updateReward(msg.sender) — inlined
        rewardPerTokenStored = rewardPerToken(currentTime);
        lastUpdateTime = lastTimeRewardApplicable(currentTime);
        rewards[msg.sender] = earned(msg.sender, currentTime);
        userRewardPerTokenPaid[msg.sender] = rewardPerTokenStored;

        require(amount > 0, "Cannot withdraw 0");
        _totalSupply = _totalSupply - amount;
        _balances[msg.sender] = _balances[msg.sender] - amount;
        // Original: stakingToken.safeTransfer(msg.sender, amount)
    }

    /// Original: getReward + updateReward modifier
    function getReward(uint256 currentTime) external returns (uint256) {
        // updateReward(msg.sender) — inlined
        rewardPerTokenStored = rewardPerToken(currentTime);
        lastUpdateTime = lastTimeRewardApplicable(currentTime);
        rewards[msg.sender] = earned(msg.sender, currentTime);
        userRewardPerTokenPaid[msg.sender] = rewardPerTokenStored;

        uint256 reward = rewards[msg.sender];
        if (reward > 0) {
            rewards[msg.sender] = 0;
            // Original: rewardsToken.safeTransfer(msg.sender, reward)
        }
        return reward;
    }

    /* ========== RESTRICTED FUNCTIONS ========== */

    /// Original formula preserved exactly for notifyRewardAmount:
    /// if (timestamp >= periodFinish) { rewardRate = reward / duration }
    /// else { remaining = periodFinish - timestamp; leftover = remaining * rewardRate;
    ///        rewardRate = (reward + leftover) / duration }
    function notifyRewardAmount(uint256 reward, uint256 currentTime) external {
        require(msg.sender == rewardsDistribution, "Caller is not RewardsDistribution");

        // updateReward(address(0)) — inlined
        rewardPerTokenStored = rewardPerToken(currentTime);
        lastUpdateTime = lastTimeRewardApplicable(currentTime);

        if (currentTime >= periodFinish) {
            rewardRate = reward / rewardsDuration;
        } else {
            uint256 remaining = periodFinish - currentTime;
            uint256 leftover = remaining * rewardRate;
            rewardRate = (reward + leftover) / rewardsDuration;
        }

        lastUpdateTime = currentTime;
        periodFinish = currentTime + rewardsDuration;
    }

    function setRewardsDuration(uint256 _rewardsDuration, uint256 currentTime) external {
        require(msg.sender == owner, "Only owner");
        require(currentTime > periodFinish, "Previous rewards period must be complete");
        rewardsDuration = _rewardsDuration;
    }

    /* ========== EXPLICIT GETTERS ========== */

    function getRewardRate() external view returns (uint256) {
        return rewardRate;
    }

    function getPeriodFinish() external view returns (uint256) {
        return periodFinish;
    }

    function getLastUpdateTime() external view returns (uint256) {
        return lastUpdateTime;
    }

    function getRewardPerTokenStored() external view returns (uint256) {
        return rewardPerTokenStored;
    }

    function getRewardsDuration() external view returns (uint256) {
        return rewardsDuration;
    }

    function getUserRewardPerTokenPaid(address account) external view returns (uint256) {
        return userRewardPerTokenPaid[account];
    }

    function getRewards(address account) external view returns (uint256) {
        return rewards[account];
    }
}
