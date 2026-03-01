// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract TokenLock {
    address private admin;
    uint256 private lockCount;
    uint256 private totalLocked;
    uint256 private totalReleased;

    mapping(uint256 => address) private _lockOwner;
    mapping(uint256 => uint256) private _lockAmount;
    mapping(uint256 => uint256) private _lockReleaseTime;
    mapping(uint256 => bool) private _lockReleased;

    constructor() {
        admin = msg.sender;
    }

    function createLock(address owner_, uint256 amount, uint256 releaseTime) public returns (uint256) {
        uint256 lockId = lockCount;
        lockCount = lockId + 1;

        _lockOwner[lockId] = owner_;
        _lockAmount[lockId] = amount;
        _lockReleaseTime[lockId] = releaseTime;
        _lockReleased[lockId] = false;

        totalLocked = totalLocked + amount;

        return lockId;
    }

    function release(uint256 lockId, uint256 currentTime) public {
        require(!_lockReleased[lockId], "Already released");
        require(currentTime >= _lockReleaseTime[lockId], "Not yet releasable");

        _lockReleased[lockId] = true;
        totalReleased = totalReleased + _lockAmount[lockId];
    }

    function getLockOwner(uint256 lockId) public view returns (address) {
        return _lockOwner[lockId];
    }

    function getLockAmount(uint256 lockId) public view returns (uint256) {
        return _lockAmount[lockId];
    }

    function getReleaseTime(uint256 lockId) public view returns (uint256) {
        return _lockReleaseTime[lockId];
    }

    function isReleased(uint256 lockId) public view returns (bool) {
        return _lockReleased[lockId];
    }

    function isReleasable(uint256 lockId, uint256 currentTime) public view returns (bool) {
        return !_lockReleased[lockId] && currentTime >= _lockReleaseTime[lockId];
    }

    function getLockCount() public view returns (uint256) {
        return lockCount;
    }

    function getTotalLocked() public view returns (uint256) {
        return totalLocked;
    }

    function getTotalReleased() public view returns (uint256) {
        return totalReleased;
    }

    function getAdmin() public view returns (address) {
        return admin;
    }
}

contract TokenLockTest is TokenLock {
    constructor() TokenLock() {}
}
