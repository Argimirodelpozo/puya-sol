// SPDX-License-Identifier: MIT
// Inspired by OpenZeppelin Governor patterns.
// Demonstrates: nested mappings, structs, modifiers, events, custom errors, enums.
pragma solidity ^0.8.20;

contract VotingTest {
    struct Proposal {
        uint256 forVotes;
        uint256 againstVotes;
        uint256 deadline;
        bool executed;
    }

    mapping(uint256 => Proposal) public proposals;
    mapping(uint256 => mapping(address => bool)) public hasVoted;
    uint256 public proposalCount;
    address public admin;

    event ProposalCreated(uint256 indexed proposalId);
    event VoteCast(uint256 indexed proposalId, bool support);
    event ProposalExecuted(uint256 indexed proposalId);

    error NotAdmin();
    error AlreadyVoted();
    error ProposalNotSucceeded();

    constructor() {
        admin = msg.sender;
    }

    modifier onlyAdmin() {
        if (msg.sender != admin) revert NotAdmin();
        _;
    }

    function createProposal(uint256 deadline) external returns (uint256) {
        uint256 id = proposalCount;
        proposalCount = id + 1;
        proposals[id] = Proposal({
            forVotes: 0,
            againstVotes: 0,
            deadline: deadline,
            executed: false
        });
        emit ProposalCreated(id);
        return id;
    }

    function vote(uint256 proposalId, bool support) external {
        if (hasVoted[proposalId][msg.sender]) revert AlreadyVoted();
        hasVoted[proposalId][msg.sender] = true;

        if (support) {
            proposals[proposalId].forVotes += 1;
        } else {
            proposals[proposalId].againstVotes += 1;
        }
        emit VoteCast(proposalId, support);
    }

    function getForVotes(uint256 proposalId) external view returns (uint256) {
        return proposals[proposalId].forVotes;
    }

    function getAgainstVotes(uint256 proposalId) external view returns (uint256) {
        return proposals[proposalId].againstVotes;
    }

    function isExecuted(uint256 proposalId) external view returns (bool) {
        return proposals[proposalId].executed;
    }

    function execute(uint256 proposalId) external onlyAdmin {
        Proposal storage p = proposals[proposalId];
        if (p.forVotes <= p.againstVotes) revert ProposalNotSucceeded();
        p.executed = true;
        emit ProposalExecuted(proposalId);
    }
}
