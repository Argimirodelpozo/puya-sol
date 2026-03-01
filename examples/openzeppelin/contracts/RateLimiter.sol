// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Rate limiting contract that controls action frequency per address.
 * Each address can perform at most maxActions within a time window.
 */
contract RateLimiterTest {
    address private _owner;
    uint256 private _maxActions;
    uint256 private _windowSize;

    mapping(address => uint256) private _actionCount;
    mapping(address => uint256) private _windowStart;

    constructor() {
        _owner = msg.sender;
        _maxActions = 5;
        _windowSize = 100; // time units
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function maxActions() external view returns (uint256) {
        return _maxActions;
    }

    function windowSize() external view returns (uint256) {
        return _windowSize;
    }

    function setMaxActions(uint256 max) external {
        require(msg.sender == _owner, "Not owner");
        _maxActions = max;
    }

    function setWindowSize(uint256 size) external {
        require(msg.sender == _owner, "Not owner");
        _windowSize = size;
    }

    function actionCount(address account) external view returns (uint256) {
        return _actionCount[account];
    }

    function windowStart(address account) external view returns (uint256) {
        return _windowStart[account];
    }

    function canPerformAction(address account, uint256 currentTime) external view returns (bool) {
        uint256 start = _windowStart[account];
        if (currentTime >= start + _windowSize) {
            return true; // new window, always allowed
        }
        return _actionCount[account] < _maxActions;
    }

    function performAction(address account, uint256 currentTime) external returns (bool) {
        uint256 start = _windowStart[account];

        if (currentTime >= start + _windowSize) {
            // Reset window
            _windowStart[account] = currentTime;
            _actionCount[account] = 1;
            return true;
        }

        require(_actionCount[account] < _maxActions, "Rate limit exceeded");
        _actionCount[account] += 1;
        return true;
    }

    function remainingActions(address account, uint256 currentTime) external view returns (uint256) {
        uint256 start = _windowStart[account];
        if (currentTime >= start + _windowSize) {
            return _maxActions; // new window
        }
        uint256 used = _actionCount[account];
        if (used >= _maxActions) return 0;
        return _maxActions - used;
    }

    function resetAccount(address account) external {
        require(msg.sender == _owner, "Not owner");
        _actionCount[account] = 0;
        _windowStart[account] = 0;
    }
}
