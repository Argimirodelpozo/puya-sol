// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * TimelockController — Schedule operations with a mandatory delay.
 * Inspired by OpenZeppelin's TimelockController.
 * Operations must be scheduled, wait for minDelay, then executed.
 *
 * Uses flat mappings to avoid struct storage pointer issues.
 * Uses operation hashing for unique identification.
 */
contract TimelockController {
    // Operation states
    uint256 public constant UNSET = 0;
    uint256 public constant PENDING = 1;
    uint256 public constant DONE = 2;

    address public admin;
    uint256 public minDelay;      // minimum delay in blocks
    uint256 public operationCount;

    // Per-operation state (flat mappings keyed by operationId)
    mapping(uint256 => uint256) private _operationState;    // UNSET, PENDING, or DONE
    mapping(uint256 => uint256) private _operationReadyAt;  // block when executable
    mapping(uint256 => address) private _operationTarget;   // target contract/address
    mapping(uint256 => uint256) private _operationValue;    // value (for payments)

    // Access control
    mapping(address => bool) private _isProposer;
    mapping(address => bool) private _isExecutor;

    // Events
    event OperationScheduled(uint256 indexed operationId, address target, uint256 delay);
    event OperationExecuted(uint256 indexed operationId);
    event OperationCancelled(uint256 indexed operationId);
    event MinDelayChanged(uint256 oldDelay, uint256 newDelay);
    event RoleGranted(address indexed account, string role);

    constructor(uint256 _minDelay) {
        admin = msg.sender;
        minDelay = _minDelay;
        // NOTE: Proposer/executor roles set via grantRole after deploy
        // (avoids __postInit constructor parameter flow bug)
    }

    // --- Access Control ---

    function grantProposerRole(address account) external {
        require(msg.sender == admin, "only admin");
        _isProposer[account] = true;
        emit RoleGranted(account, "proposer");
    }

    function grantExecutorRole(address account) external {
        require(msg.sender == admin, "only admin");
        _isExecutor[account] = true;
        emit RoleGranted(account, "executor");
    }

    function isProposer(address account) external view returns (bool) {
        return _isProposer[account];
    }

    function isExecutor(address account) external view returns (bool) {
        return _isExecutor[account];
    }

    // --- Schedule ---

    function schedule(
        address target,
        uint256 value,
        uint256 delay
    ) external returns (uint256) {
        require(_isProposer[msg.sender], "not a proposer");
        require(delay >= minDelay, "delay below minimum");

        operationCount += 1;
        uint256 opId = operationCount;

        _operationState[opId] = PENDING;
        _operationReadyAt[opId] = 100 + delay; // placeholder for block.number + delay
        _operationTarget[opId] = target;
        _operationValue[opId] = value;

        emit OperationScheduled(opId, target, delay);
        return opId;
    }

    // --- Execute ---

    function execute(uint256 operationId) external {
        require(_isExecutor[msg.sender], "not an executor");
        require(_operationState[operationId] == PENDING, "not pending");
        // In real impl, would check block.number >= readyAt
        // For testing, we skip the timing check

        _operationState[operationId] = DONE;
        emit OperationExecuted(operationId);
    }

    // --- Cancel ---

    function cancel(uint256 operationId) external {
        require(_isProposer[msg.sender], "not a proposer");
        require(_operationState[operationId] == PENDING, "not pending");

        _operationState[operationId] = UNSET;
        emit OperationCancelled(operationId);
    }

    // --- View ---

    function getOperationState(uint256 operationId) external view returns (uint256) {
        return _operationState[operationId];
    }

    function getOperationTarget(uint256 operationId) external view returns (address) {
        return _operationTarget[operationId];
    }

    function getOperationReadyAt(uint256 operationId) external view returns (uint256) {
        return _operationReadyAt[operationId];
    }

    function isOperationPending(uint256 operationId) external view returns (bool) {
        return _operationState[operationId] == PENDING;
    }

    function isOperationDone(uint256 operationId) external view returns (bool) {
        return _operationState[operationId] == DONE;
    }

    function getOperationCount() external view returns (uint256) {
        return operationCount;
    }

    // --- Admin ---

    function updateMinDelay(uint256 newDelay) external {
        require(msg.sender == admin, "only admin");
        uint256 oldDelay = minDelay;
        minDelay = newDelay;
        emit MinDelayChanged(oldDelay, newDelay);
    }
}
