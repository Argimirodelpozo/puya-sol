// SPDX-License-Identifier: MIT
// Multi-signature approval pattern.
// Demonstrates: per-proposal mappings, threshold logic, owner-only access control
pragma solidity ^0.8.20;

abstract contract MultiSig {
    address private _owner;
    uint256 private _requiredApprovals;
    uint256 private _proposalCount;
    uint256 private _totalApprovers;

    mapping(address => bool) private _isApprover;
    mapping(uint256 => uint256) private _propAmount;
    mapping(uint256 => address) private _propRecipient;
    mapping(uint256 => uint256) private _propApprovalCount;
    mapping(uint256 => bool) private _propExecuted;
    mapping(uint256 => bool) private _propCancelled;

    event ApproverAdded(address indexed approver);
    event ApproverRemoved(address indexed approver);
    event ProposalCreated(uint256 indexed proposalId, address indexed recipient, uint256 amount);
    event ProposalApproved(uint256 indexed proposalId);
    event ProposalExecuted(uint256 indexed proposalId);
    event ProposalCancelled(uint256 indexed proposalId);

    error NotOwner();
    error NotApprover();
    error ProposalAlreadyExecuted();
    error ProposalAlreadyCancelled();
    error InsufficientApprovals();

    constructor(uint256 requiredApprovals) {
        _owner = msg.sender;
        _requiredApprovals = requiredApprovals;
        _proposalCount = 0;
        _totalApprovers = 0;
    }

    function addApprover(address approver) external {
        if (msg.sender != _owner) revert NotOwner();
        _isApprover[approver] = true;
        _totalApprovers += 1;
        emit ApproverAdded(approver);
    }

    function removeApprover(address approver) external {
        if (msg.sender != _owner) revert NotOwner();
        _isApprover[approver] = false;
        _totalApprovers -= 1;
        emit ApproverRemoved(approver);
    }

    function isApprover(address addr) external view returns (bool) {
        return _isApprover[addr];
    }

    function propose(address recipient, uint256 amount) external returns (uint256) {
        _proposalCount += 1;
        uint256 proposalId = _proposalCount;
        _propAmount[proposalId] = amount;
        _propRecipient[proposalId] = recipient;
        _propApprovalCount[proposalId] = 0;
        _propExecuted[proposalId] = false;
        _propCancelled[proposalId] = false;
        emit ProposalCreated(proposalId, recipient, amount);
        return proposalId;
    }

    function approve(uint256 proposalId) external {
        if (_propExecuted[proposalId]) revert ProposalAlreadyExecuted();
        if (_propCancelled[proposalId]) revert ProposalAlreadyCancelled();
        _propApprovalCount[proposalId] += 1;
        emit ProposalApproved(proposalId);
    }

    function executeProposal(uint256 proposalId) external returns (bool) {
        if (_propExecuted[proposalId]) revert ProposalAlreadyExecuted();
        if (_propCancelled[proposalId]) revert ProposalAlreadyCancelled();
        if (_propApprovalCount[proposalId] < _requiredApprovals) revert InsufficientApprovals();
        _propExecuted[proposalId] = true;
        emit ProposalExecuted(proposalId);
        return true;
    }

    function cancelProposal(uint256 proposalId) external {
        if (msg.sender != _owner) revert NotOwner();
        if (_propExecuted[proposalId]) revert ProposalAlreadyExecuted();
        _propCancelled[proposalId] = true;
        emit ProposalCancelled(proposalId);
    }

    function getProposalAmount(uint256 id) external view returns (uint256) {
        return _propAmount[id];
    }

    function getProposalRecipient(uint256 id) external view returns (address) {
        return _propRecipient[id];
    }

    function getApprovalCount(uint256 id) external view returns (uint256) {
        return _propApprovalCount[id];
    }

    function isExecuted(uint256 id) external view returns (bool) {
        return _propExecuted[id];
    }

    function isCancelled(uint256 id) external view returns (bool) {
        return _propCancelled[id];
    }

    function getOwner() external view returns (address) {
        return _owner;
    }

    // Initialize mapping boxes for an approver (AVM boxes must be created before read)
    function initApprover(address approver) external {
        _isApprover[approver] = false;
    }

    // Initialize mapping boxes for a proposal (AVM boxes must be created before read)
    function initProposal(uint256 proposalId) external {
        _propAmount[proposalId] = 0;
        _propRecipient[proposalId] = address(0);
        _propApprovalCount[proposalId] = 0;
        _propExecuted[proposalId] = false;
        _propCancelled[proposalId] = false;
    }
}

// Test contract — wraps MultiSig with requiredApprovals=2
contract MultiSigTest is MultiSig {
    constructor() MultiSig(2) {}
}
