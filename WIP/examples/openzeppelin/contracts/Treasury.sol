// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simplified treasury with approval tracking.
 * Proposals require multiple approvals before execution.
 */
contract TreasuryTest {
    address private _admin;
    uint256 private _proposalCount;
    uint256 private _requiredApprovals;
    uint256 private _totalFunds;

    mapping(uint256 => address) private _proposalCreator;
    mapping(uint256 => uint256) private _proposalAmount;
    mapping(uint256 => address) private _proposalRecipient;
    mapping(uint256 => uint256) private _proposalApprovals;
    mapping(uint256 => bool) private _proposalExecuted;

    constructor() {
        _admin = msg.sender;
        _requiredApprovals = 2;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function proposalCount() external view returns (uint256) {
        return _proposalCount;
    }

    function requiredApprovals() external view returns (uint256) {
        return _requiredApprovals;
    }

    function totalFunds() external view returns (uint256) {
        return _totalFunds;
    }

    function setRequiredApprovals(uint256 count) external {
        require(msg.sender == _admin, "Not admin");
        require(count > 0, "Must be > 0");
        _requiredApprovals = count;
    }

    function deposit(uint256 amount) external {
        require(amount > 0, "Amount must be > 0");
        _totalFunds += amount;
    }

    function createProposal(address recipient, uint256 amount) external returns (uint256) {
        require(amount > 0, "Amount must be > 0");

        _proposalCount += 1;
        uint256 id = _proposalCount;

        _proposalCreator[id] = msg.sender;
        _proposalAmount[id] = amount;
        _proposalRecipient[id] = recipient;
        _proposalApprovals[id] = 0;
        _proposalExecuted[id] = false;

        return id;
    }

    function approve(uint256 proposalId) external {
        require(_proposalCreator[proposalId] != address(0), "Proposal not found");
        require(!_proposalExecuted[proposalId], "Already executed");
        _proposalApprovals[proposalId] += 1;
    }

    function executeProposal(uint256 proposalId) external returns (bool) {
        require(_proposalCreator[proposalId] != address(0), "Proposal not found");
        require(!_proposalExecuted[proposalId], "Already executed");
        require(
            _proposalApprovals[proposalId] >= _requiredApprovals,
            "Not enough approvals"
        );

        uint256 amount = _proposalAmount[proposalId];
        require(_totalFunds >= amount, "Insufficient funds");

        _proposalExecuted[proposalId] = true;
        _totalFunds -= amount;

        return true;
    }

    function getProposalAmount(uint256 proposalId) external view returns (uint256) {
        return _proposalAmount[proposalId];
    }

    function getProposalRecipient(uint256 proposalId) external view returns (address) {
        return _proposalRecipient[proposalId];
    }

    function getProposalApprovals(uint256 proposalId) external view returns (uint256) {
        return _proposalApprovals[proposalId];
    }

    function isProposalExecuted(uint256 proposalId) external view returns (bool) {
        return _proposalExecuted[proposalId];
    }
}
