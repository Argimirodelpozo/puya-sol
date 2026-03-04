// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/**
 * @title MultiSigWallet
 * @notice A multi-signature wallet requiring M-of-N confirmations.
 * Exercises: nested mappings, counter patterns, array-like indexed storage,
 * multi-step state transitions, role checking via mapping lookups.
 */
contract MultiSigWallet {
    // ─── State Variables ───
    address public owner;
    uint256 public required;       // confirmations required
    uint256 public signerCount;    // number of signers
    uint256 public txCount;        // total transactions submitted

    // Signer management (flat mappings)
    mapping(address => bool) private _isSigner;

    // Transaction fields (flat mappings instead of struct)
    mapping(uint256 => address) private _txDestination;
    mapping(uint256 => uint256) private _txValue;
    mapping(uint256 => bool) private _txExecuted;
    mapping(uint256 => uint256) private _txConfirmationCount;

    // Nested mapping: txId => signer => confirmed
    mapping(uint256 => mapping(address => bool)) private _confirmations;

    // ─── Events ───
    event SignerAdded(address indexed signer);
    event SignerRemoved(address indexed signer);
    event TransactionSubmitted(uint256 indexed txId, address indexed destination, uint256 value);
    event TransactionConfirmed(uint256 indexed txId, address indexed signer);
    event ConfirmationRevoked(uint256 indexed txId, address indexed signer);
    event TransactionExecuted(uint256 indexed txId);
    event RequirementChanged(uint256 required);

    // ─── Constructor ───
    constructor(uint256 _required) {
        owner = msg.sender;
        required = _required;
    }

    // ─── Signer Management ───

    function addSigner(address signer) external {
        require(msg.sender == owner, "not owner");
        require(!_isSigner[signer], "already signer");
        _isSigner[signer] = true;
        signerCount = signerCount + 1;
    }

    function removeSigner(address signer) external {
        require(msg.sender == owner, "not owner");
        require(_isSigner[signer], "not signer");
        _isSigner[signer] = false;
        signerCount = signerCount - 1;
        // Ensure required doesn't exceed signer count
        if (required > signerCount) {
            required = signerCount;
        }
    }

    function isSigner(address account) external view returns (bool) {
        return _isSigner[account];
    }

    // ─── Transaction Lifecycle ───

    function submitTransaction(address destination, uint256 value) external returns (uint256) {
        require(_isSigner[msg.sender], "not signer");
        txCount = txCount + 1;
        uint256 txId = txCount;
        _txDestination[txId] = destination;
        _txValue[txId] = value;
        _txExecuted[txId] = false;
        _txConfirmationCount[txId] = 0;
        return txId;
    }

    function confirmTransaction(uint256 txId) external {
        require(_isSigner[msg.sender], "not signer");
        require(txId >= 1 && txId <= txCount, "tx does not exist");
        require(!_txExecuted[txId], "already executed");
        require(!_confirmations[txId][msg.sender], "already confirmed");

        _confirmations[txId][msg.sender] = true;
        _txConfirmationCount[txId] = _txConfirmationCount[txId] + 1;
    }

    function revokeConfirmation(uint256 txId) external {
        require(_isSigner[msg.sender], "not signer");
        require(txId >= 1 && txId <= txCount, "tx does not exist");
        require(!_txExecuted[txId], "already executed");
        require(_confirmations[txId][msg.sender], "not confirmed");

        _confirmations[txId][msg.sender] = false;
        _txConfirmationCount[txId] = _txConfirmationCount[txId] - 1;
    }

    function executeTransaction(uint256 txId) external {
        require(_isSigner[msg.sender], "not signer");
        require(txId >= 1 && txId <= txCount, "tx does not exist");
        require(!_txExecuted[txId], "already executed");
        require(_txConfirmationCount[txId] >= required, "not enough confirmations");

        _txExecuted[txId] = true;
        // In a real wallet, this would send a payment inner txn
        // For now, we just mark as executed
    }

    // ─── View Functions ───

    function getTransactionCount() external view returns (uint256) {
        return txCount;
    }

    function getConfirmationCount(uint256 txId) external view returns (uint256) {
        return _txConfirmationCount[txId];
    }

    function isConfirmed(uint256 txId, address signer) external view returns (bool) {
        return _confirmations[txId][signer];
    }

    function isExecuted(uint256 txId) external view returns (bool) {
        return _txExecuted[txId];
    }

    function getDestination(uint256 txId) external view returns (address) {
        return _txDestination[txId];
    }

    function getValue(uint256 txId) external view returns (uint256) {
        return _txValue[txId];
    }

    function changeRequirement(uint256 _required) external {
        require(msg.sender == owner, "not owner");
        require(_required <= signerCount, "exceeds signer count");
        require(_required >= 1, "at least 1 required");
        required = _required;
    }

    // Explicit getters (public state vars don't generate ARC56 getters)
    function getRequired() external view returns (uint256) {
        return required;
    }

    function getSignerCount() external view returns (uint256) {
        return signerCount;
    }

    function getOwner() external view returns (address) {
        return owner;
    }
}
