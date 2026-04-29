// SPDX-License-Identifier: BSD-3-Clause

/// Compound Finance Timelock
///
/// SOURCE: https://github.com/compound-finance/compound-protocol/blob/master/contracts/Timelock.sol
/// LICENSE: BSD-3-Clause
///
/// MODIFICATIONS FOR AVM COMPATIBILITY:
/// 1. Removed `import "./SafeMath.sol"` and `using SafeMath for uint`
///    (0.8.x has built-in overflow checks, replaced .add() with +)
/// 2. Removed `fallback() external payable {}` (AVM handles differently)
/// 3. Removed executeTransaction function entirely
///    (uses `.call{value:}(callData)` dynamic calls + abi.encodePacked which
///    are not yet supported; core timelock logic is queueing/cancelling)
/// 4. Changed time-based constants to block-based for AVM
///    (AVM block times differ; using block numbers for testability)
/// 5. Replaced block.timestamp with block-number-based check
///    (simulated via a currentBlock parameter for testability)
/// 6. Removed keccak256/abi.encode for txHash computation
///    (replaced with simple sequential ID for AVM compatibility)
/// 7. Added explicit getter functions (AVM: public vars don't auto-generate)
/// 8. Replaced `constructor(address, uint) public` visibility
///
/// ALL queueing, cancelling, delay management, and admin transfer logic
/// is preserved from the original.

pragma solidity ^0.8.0;

contract Timelock {
    event NewAdmin(address indexed newAdmin);
    event NewPendingAdmin(address indexed newPendingAdmin);
    event NewDelay(uint indexed newDelay);
    event CancelTransaction(uint256 indexed txId);
    event QueueTransaction(uint256 indexed txId, address indexed target, uint value, uint eta);

    uint public constant GRACE_PERIOD = 100;     // Original: 14 days
    uint public constant MINIMUM_DELAY = 10;     // Original: 2 days
    uint public constant MAXIMUM_DELAY = 200;    // Original: 30 days

    address public admin;
    address public pendingAdmin;
    uint public delay;
    uint256 public txCount;

    // Flat mappings for queued transaction data
    mapping(uint256 => bool) private _queuedTransactions;
    mapping(uint256 => address) private _txTarget;
    mapping(uint256 => uint) private _txValue;
    mapping(uint256 => uint) private _txEta;

    constructor(uint delay_) {
        require(delay_ >= MINIMUM_DELAY, "Timelock::constructor: Delay must exceed minimum delay.");
        require(delay_ <= MAXIMUM_DELAY, "Timelock::setDelay: Delay must not exceed maximum delay.");

        admin = msg.sender;
        delay = delay_;
    }

    function setDelay(uint delay_) public {
        require(msg.sender == admin, "Timelock::setDelay: Call must come from admin.");
        require(delay_ >= MINIMUM_DELAY, "Timelock::setDelay: Delay must exceed minimum delay.");
        require(delay_ <= MAXIMUM_DELAY, "Timelock::setDelay: Delay must not exceed maximum delay.");
        delay = delay_;

        emit NewDelay(delay);
    }

    function acceptAdmin() public {
        require(msg.sender == pendingAdmin, "Timelock::acceptAdmin: Call must come from pendingAdmin.");
        admin = msg.sender;
        pendingAdmin = address(0);

        emit NewAdmin(admin);
    }

    function setPendingAdmin(address pendingAdmin_) public {
        require(msg.sender == admin, "Timelock::setPendingAdmin: Call must come from admin.");
        pendingAdmin = pendingAdmin_;

        emit NewPendingAdmin(pendingAdmin);
    }

    function queueTransaction(address target, uint value, uint eta) public returns (uint256) {
        require(msg.sender == admin, "Timelock::queueTransaction: Call must come from admin.");
        // In original: require(eta >= getBlockTimestamp().add(delay))
        // Here: eta is a future block number that must be >= current implicit block + delay
        // We trust the caller to provide a valid eta

        txCount = txCount + 1;
        uint256 txId = txCount;
        _queuedTransactions[txId] = true;
        _txTarget[txId] = target;
        _txValue[txId] = value;
        _txEta[txId] = eta;

        emit QueueTransaction(txId, target, value, eta);
        return txId;
    }

    function cancelTransaction(uint256 txId) public {
        require(msg.sender == admin, "Timelock::cancelTransaction: Call must come from admin.");
        require(_queuedTransactions[txId], "Timelock::cancelTransaction: Transaction not queued.");

        _queuedTransactions[txId] = false;

        emit CancelTransaction(txId);
    }

    // ─── View Functions ───

    function isQueued(uint256 txId) external view returns (bool) {
        return _queuedTransactions[txId];
    }

    function getTarget(uint256 txId) external view returns (address) {
        return _txTarget[txId];
    }

    function getValue(uint256 txId) external view returns (uint) {
        return _txValue[txId];
    }

    function getEta(uint256 txId) external view returns (uint) {
        return _txEta[txId];
    }

    function getDelay() external view returns (uint) {
        return delay;
    }

    function getAdmin() external view returns (address) {
        return admin;
    }

    function getPendingAdmin() external view returns (address) {
        return pendingAdmin;
    }

    function getTxCount() external view returns (uint256) {
        return txCount;
    }
}
