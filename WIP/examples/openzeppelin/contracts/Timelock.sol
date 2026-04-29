// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Timelock pattern for delayed execution of operations.
 * Each operation is identified by a uint256 operationId (auto-incremented).
 * Operations must wait a minimum delay before execution.
 * All numeric values use uint256 (mapped to biguint on AVM).
 * Per-operation state stored in mappings (boxes on AVM).
 */
abstract contract Timelock {
    address private _admin;
    uint256 private _minDelay;
    uint256 private _operationCount;

    // Per-operation state keyed by operationId (internal for test wrapper box init)
    mapping(uint256 => uint256) internal _opTimestamp;
    mapping(uint256 => bool) internal _opExecuted;
    mapping(uint256 => uint256) internal _opTarget;
    mapping(uint256 => uint256) internal _opValue;

    event OperationScheduled(uint256 operationId, uint256 target, uint256 value, uint256 timestamp);
    event OperationExecuted(uint256 operationId);
    event OperationCancelled(uint256 operationId);
    event MinDelayChanged(uint256 oldDelay, uint256 newDelay);

    constructor(uint256 minDelay) {
        _admin = msg.sender;
        _minDelay = minDelay;
        _operationCount = 0;
    }

    /**
     * @dev Schedule an operation with a delay. Only admin.
     * currentTime is passed as a parameter (AVM has no block.timestamp equivalent).
     * Returns the assigned operationId.
     */
    function schedule(
        uint256 target,
        uint256 value,
        uint256 delay,
        uint256 currentTime
    ) external returns (uint256) {
        require(msg.sender == _admin, "Timelock: caller is not admin");
        require(delay >= _minDelay, "Timelock: insufficient delay");

        uint256 operationId = _operationCount;
        _operationCount = operationId + 1;

        uint256 timestamp = currentTime + delay;
        _opTimestamp[operationId] = timestamp;
        _opExecuted[operationId] = false;
        _opTarget[operationId] = target;
        _opValue[operationId] = value;

        emit OperationScheduled(operationId, target, value, timestamp);

        return operationId;
    }

    /**
     * @dev Execute a scheduled operation. Only admin.
     * Requires the operation is not yet executed and the delay has passed.
     */
    function execute(uint256 operationId, uint256 currentTime) external returns (bool) {
        require(msg.sender == _admin, "Timelock: caller is not admin");
        require(!_opExecuted[operationId], "Timelock: already executed");

        uint256 timestamp = _opTimestamp[operationId];
        require(timestamp != 0, "Timelock: operation not scheduled");
        require(currentTime >= timestamp, "Timelock: not yet ready");

        _opExecuted[operationId] = true;

        emit OperationExecuted(operationId);

        return true;
    }

    /**
     * @dev Cancel a pending operation. Only admin.
     * Sets the timestamp to 0, effectively cancelling it.
     */
    function cancel(uint256 operationId) external {
        require(msg.sender == _admin, "Timelock: caller is not admin");
        require(!_opExecuted[operationId], "Timelock: already executed");

        uint256 timestamp = _opTimestamp[operationId];
        require(timestamp != 0, "Timelock: operation not scheduled");

        _opTimestamp[operationId] = 0;

        emit OperationCancelled(operationId);
    }

    /**
     * @dev Returns true if the operation is ready to execute.
     */
    function isOperationReady(uint256 operationId, uint256 currentTime) external view returns (bool) {
        uint256 timestamp = _opTimestamp[operationId];
        return timestamp != 0 && !_opExecuted[operationId] && currentTime >= timestamp;
    }

    /**
     * @dev Returns true if the operation has been executed.
     */
    function isOperationDone(uint256 operationId) external view returns (bool) {
        return _opExecuted[operationId];
    }

    /**
     * @dev Returns the scheduled timestamp for an operation.
     */
    function getTimestamp(uint256 operationId) external view returns (uint256) {
        return _opTimestamp[operationId];
    }

    /**
     * @dev Returns the admin address.
     */
    function getAdmin() external view returns (address) {
        return _admin;
    }

    /**
     * @dev Returns the minimum delay.
     */
    function getMinDelay() external view returns (uint256) {
        return _minDelay;
    }

    /**
     * @dev Updates the minimum delay. Only admin.
     */
    function setMinDelay(uint256 newDelay) external {
        require(msg.sender == _admin, "Timelock: caller is not admin");
        uint256 oldDelay = _minDelay;
        _minDelay = newDelay;
        emit MinDelayChanged(oldDelay, newDelay);
    }
}

/**
 * @dev Test wrapper for Timelock with minDelay=10.
 */
contract TimelockTest is Timelock(10) {
    // Expose internal state for testing

    function initOperation(uint256 operationId) external {
        // Initialize mapping boxes for an operationId (AVM boxes must exist before read+write)
        _opTimestamp[operationId] = 0;
        _opExecuted[operationId] = false;
        _opTarget[operationId] = 0;
        _opValue[operationId] = 0;
    }
}
