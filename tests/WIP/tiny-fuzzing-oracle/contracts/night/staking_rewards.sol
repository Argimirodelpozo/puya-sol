// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract StakingRewards {
    uint256 public rewardRate = 100;
    uint256 public lastUpdateTime;
    uint256 public rewardPerTokenStored;
    uint256 public totalSupply;
    mapping(address => uint256) public userRewardPerTokenPaid;
    mapping(address => uint256) public rewards;
    mapping(address => uint256) public balanceOf;

    function rewardPerToken() public view returns (uint256) {
        if (totalSupply == 0) return rewardPerTokenStored;
        return rewardPerTokenStored + (((block.timestamp - lastUpdateTime) * rewardRate * 1e18) / totalSupply);
    }
    function earned(address a) public view returns (uint256) {
        return ((balanceOf[a] * (rewardPerToken() - userRewardPerTokenPaid[a])) / 1e18) + rewards[a];
    }
    function _update(address a) internal {
        rewardPerTokenStored = rewardPerToken();
        lastUpdateTime = block.timestamp;
        rewards[a] = earned(a);
        userRewardPerTokenPaid[a] = rewardPerTokenStored;
    }
    function stake(uint256 amt) external {
        _update(msg.sender);
        totalSupply += amt;
        balanceOf[msg.sender] += amt;
    }
    function withdraw(uint256 amt) external {
        _update(msg.sender);
        totalSupply -= amt;
        balanceOf[msg.sender] -= amt;
    }
    function getReward() external returns (uint256) {
        _update(msg.sender);
        uint256 r = rewards[msg.sender];
        rewards[msg.sender] = 0;
        return r;
    }
}
