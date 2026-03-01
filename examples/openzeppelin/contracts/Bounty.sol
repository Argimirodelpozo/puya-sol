// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Bounty {
    address private _admin;
    uint256 private _bountyCount;
    uint256 private _totalRewardPool;
    uint256 private _totalPaid;
    uint256 private _claimCount;

    mapping(uint256 => uint256) internal _bountyReward;
    mapping(uint256 => bool) internal _bountyActive;
    mapping(uint256 => uint256) internal _bountyClaimCount;

    mapping(uint256 => uint256) internal _claimBountyId;
    mapping(uint256 => address) internal _claimHunter;
    mapping(uint256 => uint256) internal _claimStatus;

    constructor() {
        _admin = msg.sender;
        _bountyCount = 0;
        _totalRewardPool = 0;
        _totalPaid = 0;
        _claimCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getBountyCount() external view returns (uint256) {
        return _bountyCount;
    }

    function getTotalRewardPool() external view returns (uint256) {
        return _totalRewardPool;
    }

    function getTotalPaid() external view returns (uint256) {
        return _totalPaid;
    }

    function getClaimCount() external view returns (uint256) {
        return _claimCount;
    }

    function getBountyReward(uint256 bountyId) external view returns (uint256) {
        return _bountyReward[bountyId];
    }

    function isBountyActive(uint256 bountyId) external view returns (bool) {
        return _bountyActive[bountyId];
    }

    function getBountyClaimCount(uint256 bountyId) external view returns (uint256) {
        return _bountyClaimCount[bountyId];
    }

    function getClaimBountyId(uint256 claimId) external view returns (uint256) {
        return _claimBountyId[claimId];
    }

    function getClaimHunter(uint256 claimId) external view returns (address) {
        return _claimHunter[claimId];
    }

    function getClaimStatus(uint256 claimId) external view returns (uint256) {
        return _claimStatus[claimId];
    }

    function createBounty(uint256 reward) external returns (uint256) {
        uint256 bountyId = _bountyCount;
        _bountyReward[bountyId] = reward;
        _bountyActive[bountyId] = true;
        _bountyClaimCount[bountyId] = 0;
        _totalRewardPool = _totalRewardPool + reward;
        _bountyCount = _bountyCount + 1;
        return bountyId;
    }

    function closeBounty(uint256 bountyId) external {
        require(_bountyActive[bountyId] == true, "Bounty not active");
        _bountyActive[bountyId] = false;
    }

    function reopenBounty(uint256 bountyId) external {
        require(_bountyActive[bountyId] == false, "Bounty already active");
        _bountyActive[bountyId] = true;
    }

    function submitClaim(uint256 bountyId, address hunter) external returns (uint256) {
        require(_bountyActive[bountyId] == true, "Bounty not active");
        uint256 claimId = _claimCount;
        _claimBountyId[claimId] = bountyId;
        _claimHunter[claimId] = hunter;
        _claimStatus[claimId] = 0;
        _bountyClaimCount[bountyId] = _bountyClaimCount[bountyId] + 1;
        _claimCount = _claimCount + 1;
        return claimId;
    }

    function approveClaim(uint256 claimId) external {
        require(_claimStatus[claimId] == 0, "Claim not pending");
        _claimStatus[claimId] = 1;
        uint256 bountyId = _claimBountyId[claimId];
        uint256 reward = _bountyReward[bountyId];
        _totalPaid = _totalPaid + reward;
    }

    function rejectClaim(uint256 claimId) external {
        require(_claimStatus[claimId] == 0, "Claim not pending");
        _claimStatus[claimId] = 2;
    }
}

contract BountyTest is Bounty {
    constructor() Bounty() {}

    function initBounty(uint256 bountyId) external {
        _bountyReward[bountyId] = 0;
        _bountyActive[bountyId] = false;
        _bountyClaimCount[bountyId] = 0;
    }

    function initClaim(uint256 claimId) external {
        _claimBountyId[claimId] = 0;
        _claimHunter[claimId] = address(0);
        _claimStatus[claimId] = 0;
    }
}
