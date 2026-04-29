// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simple governance contract with proposal creation, voting, and execution.
 * Voting power is tracked per-address. Proposals have FOR/AGAINST/ABSTAIN votes.
 */
contract GovernanceTest {
    address private _admin;
    uint256 private _proposalCount;
    uint256 private _votingPeriod;
    uint256 private _quorum; // minimum total votes needed

    mapping(address => uint256) private _votingPower;
    mapping(uint256 => uint256) private _forVotes;
    mapping(uint256 => uint256) private _againstVotes;
    mapping(uint256 => uint256) private _abstainVotes;
    mapping(uint256 => bool) private _proposalExecuted;
    mapping(uint256 => uint256) private _proposalDeadline;

    constructor() {
        _admin = msg.sender;
        _votingPeriod = 100;
        _quorum = 10;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function proposalCount() external view returns (uint256) {
        return _proposalCount;
    }

    function votingPeriod() external view returns (uint256) {
        return _votingPeriod;
    }

    function quorum() external view returns (uint256) {
        return _quorum;
    }

    function setVotingPower(address account, uint256 power) external {
        require(msg.sender == _admin, "Not admin");
        _votingPower[account] = power;
    }

    function getVotingPower(address account) external view returns (uint256) {
        return _votingPower[account];
    }

    function createProposal(uint256 currentTime) external returns (uint256) {
        _proposalCount += 1;
        uint256 id = _proposalCount;
        _proposalDeadline[id] = currentTime + _votingPeriod;
        return id;
    }

    function castVote(uint256 proposalId, uint256 support, address voter) external {
        require(_votingPower[voter] > 0, "No voting power");
        uint256 weight = _votingPower[voter];

        if (support == 0) {
            _againstVotes[proposalId] += weight;
        } else if (support == 1) {
            _forVotes[proposalId] += weight;
        } else {
            _abstainVotes[proposalId] += weight;
        }
    }

    function forVotes(uint256 proposalId) external view returns (uint256) {
        return _forVotes[proposalId];
    }

    function againstVotes(uint256 proposalId) external view returns (uint256) {
        return _againstVotes[proposalId];
    }

    function abstainVotes(uint256 proposalId) external view returns (uint256) {
        return _abstainVotes[proposalId];
    }

    function proposalDeadline(uint256 proposalId) external view returns (uint256) {
        return _proposalDeadline[proposalId];
    }

    function isProposalPassed(uint256 proposalId) external view returns (bool) {
        uint256 totalVotes = _forVotes[proposalId] + _againstVotes[proposalId] + _abstainVotes[proposalId];
        if (totalVotes < _quorum) return false;
        return _forVotes[proposalId] > _againstVotes[proposalId];
    }

    function executeProposal(uint256 proposalId, uint256 currentTime) external returns (bool) {
        require(!_proposalExecuted[proposalId], "Already executed");
        require(currentTime >= _proposalDeadline[proposalId], "Voting not ended");

        uint256 totalVotes = _forVotes[proposalId] + _againstVotes[proposalId] + _abstainVotes[proposalId];
        require(totalVotes >= _quorum, "Quorum not reached");
        require(_forVotes[proposalId] > _againstVotes[proposalId], "Proposal not passed");

        _proposalExecuted[proposalId] = true;
        return true;
    }

    function isProposalExecuted(uint256 proposalId) external view returns (bool) {
        return _proposalExecuted[proposalId];
    }
}
