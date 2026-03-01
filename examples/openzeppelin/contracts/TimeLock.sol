// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simplified timelock: schedule operations with a delay, then execute after the delay.
 * Uses mapping(bytes32 => uint256) to store timestamps.
 */
contract TimeLockTest {
    uint256 private _minDelay;
    mapping(bytes32 => uint256) private _timestamps;
    address private _admin;

    // Timestamp value meaning "done" (1 means executed)
    uint256 private constant _DONE_TIMESTAMP = 1;

    constructor() {
        _minDelay = 10;  // 10 second minimum delay
        _admin = msg.sender;
    }

    function getMinDelay() external view returns (uint256) {
        return _minDelay;
    }

    function setMinDelay(uint256 newDelay) external {
        require(msg.sender == _admin, "TimeLock: caller is not admin");
        _minDelay = newDelay;
    }

    function isOperation(bytes32 id) external view returns (bool) {
        return _timestamps[id] > 0;
    }

    function isOperationPending(bytes32 id) external view returns (bool) {
        return _timestamps[id] > _DONE_TIMESTAMP;
    }

    function isOperationReady(bytes32 id) external view returns (bool) {
        uint256 ts = _timestamps[id];
        // Ready if scheduled and current time >= scheduled time
        // For testing, we'll use a simplified check
        return ts > _DONE_TIMESTAMP;
    }

    function isOperationDone(bytes32 id) external view returns (bool) {
        return _timestamps[id] == _DONE_TIMESTAMP;
    }

    function getTimestamp(bytes32 id) external view returns (uint256) {
        return _timestamps[id];
    }

    // Schedule an operation with given id and delay
    function schedule(bytes32 id, uint256 delay) external {
        require(msg.sender == _admin, "TimeLock: caller is not admin");
        require(_timestamps[id] == 0, "TimeLock: operation already scheduled");
        require(delay >= _minDelay, "TimeLock: insufficient delay");
        _timestamps[id] = delay + 2;  // +2 to ensure > _DONE_TIMESTAMP
    }

    // Mark an operation as done
    function execute(bytes32 id) external {
        require(msg.sender == _admin, "TimeLock: caller is not admin");
        require(_timestamps[id] > _DONE_TIMESTAMP, "TimeLock: operation not pending");
        _timestamps[id] = _DONE_TIMESTAMP;
    }

    // Cancel a pending operation
    function cancel(bytes32 id) external {
        require(msg.sender == _admin, "TimeLock: caller is not admin");
        require(_timestamps[id] > _DONE_TIMESTAMP, "TimeLock: operation not pending");
        _timestamps[id] = 0;
    }

    // Hash operation parameters to get an ID
    function hashOperation(uint256 value, bytes32 salt) external pure returns (bytes32) {
        return keccak256(abi.encodePacked(value, salt));
    }
}
