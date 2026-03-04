// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * Governor — Compound-inspired on-chain governance.
 * Supports proposal creation, voting, and execution.
 * Uses an external governance token for voting power.
 *
 * NOTE: Uses flat mappings instead of struct mappings to work around
 * the struct-storage-field-mutation compiler limitation.
 */

interface IGovernorToken {
    function getVotingPower(address account) external view returns (uint256);
    function totalSupply() external view returns (uint256);
}

contract Governor {
    // --- State ---
    IGovernorToken public token;
    address public admin;
    uint256 public proposalCount;
    uint256 public quorumVotes;
    uint256 public proposalThreshold;

    // Proposal fields as separate mappings (avoids struct storage pointer issue)
    mapping(uint256 => address) private _proposalProposer;
    mapping(uint256 => uint256) private _proposalForVotes;
    mapping(uint256 => uint256) private _proposalAgainstVotes;
    mapping(uint256 => uint256) private _proposalAbstainVotes;
    mapping(uint256 => bool) private _proposalExecuted;
    mapping(uint256 => bool) private _proposalCancelled;

    // Receipt fields as separate nested mappings
    mapping(uint256 => mapping(address => bool)) private _receiptHasVoted;
    mapping(uint256 => mapping(address => uint256)) private _receiptSupport;
    mapping(uint256 => mapping(address => uint256)) private _receiptVotes;

    // --- Events ---
    event ProposalCreated(uint256 indexed proposalId, address proposer, string description);
    event VoteCast(address indexed voter, uint256 indexed proposalId, uint8 support, uint256 votes);
    event ProposalExecuted(uint256 indexed proposalId);
    event ProposalCancelled(uint256 indexed proposalId);

    constructor(
        address _token,
        uint256 _quorumVotes,
        uint256 _proposalThreshold
    ) {
        token = IGovernorToken(_token);
        admin = msg.sender;
        quorumVotes = _quorumVotes;
        proposalThreshold = _proposalThreshold;
    }

    /// Create a new proposal. Proposer must have enough voting power.
    function propose(string memory description) external returns (uint256) {
        uint256 proposerVotes = token.getVotingPower(msg.sender);
        require(proposerVotes >= proposalThreshold, "below proposal threshold");

        proposalCount += 1;
        uint256 proposalId = proposalCount;

        _proposalProposer[proposalId] = msg.sender;

        emit ProposalCreated(proposalId, msg.sender, description);
        return proposalId;
    }

    /// Cast a vote. Support: 0=Against, 1=For, 2=Abstain.
    function castVote(uint256 proposalId, uint8 support) external returns (uint256) {
        require(proposalId > 0 && proposalId <= proposalCount, "invalid proposal");
        require(support <= 2, "invalid vote type");
        require(!_receiptHasVoted[proposalId][msg.sender], "already voted");

        uint256 votes = token.getVotingPower(msg.sender);
        require(votes > 0, "no voting power");

        _receiptHasVoted[proposalId][msg.sender] = true;
        _receiptSupport[proposalId][msg.sender] = support;
        _receiptVotes[proposalId][msg.sender] = votes;

        if (support == 0) {
            _proposalAgainstVotes[proposalId] += votes;
        } else if (support == 1) {
            _proposalForVotes[proposalId] += votes;
        } else {
            _proposalAbstainVotes[proposalId] += votes;
        }

        emit VoteCast(msg.sender, proposalId, support, votes);
        return votes;
    }

    /// Execute a succeeded proposal.
    function execute(uint256 proposalId) external {
        require(proposalId > 0 && proposalId <= proposalCount, "invalid proposal");
        require(!_proposalExecuted[proposalId], "already executed");
        require(!_proposalCancelled[proposalId], "proposal cancelled");
        require(_proposalForVotes[proposalId] > _proposalAgainstVotes[proposalId], "proposal not succeeded");
        require(_proposalForVotes[proposalId] + _proposalAbstainVotes[proposalId] >= quorumVotes, "quorum not reached");

        _proposalExecuted[proposalId] = true;
        emit ProposalExecuted(proposalId);
    }

    /// Cancel a proposal. Only proposer or admin can cancel.
    function cancel(uint256 proposalId) external {
        require(proposalId > 0 && proposalId <= proposalCount, "invalid proposal");
        require(!_proposalExecuted[proposalId], "already executed");
        require(!_proposalCancelled[proposalId], "already cancelled");
        require(
            msg.sender == _proposalProposer[proposalId] || msg.sender == admin,
            "not proposer or admin"
        );

        _proposalCancelled[proposalId] = true;
        emit ProposalCancelled(proposalId);
    }

    // --- View functions ---

    function getProposal(uint256 proposalId) external view returns (
        address proposer,
        uint256 forVotes,
        uint256 againstVotes,
        uint256 abstainVotes,
        bool executed,
        bool cancelled
    ) {
        return (
            _proposalProposer[proposalId],
            _proposalForVotes[proposalId],
            _proposalAgainstVotes[proposalId],
            _proposalAbstainVotes[proposalId],
            _proposalExecuted[proposalId],
            _proposalCancelled[proposalId]
        );
    }

    function getReceipt(uint256 proposalId, address voter) external view returns (
        bool hasVoted,
        uint256 support,
        uint256 votes
    ) {
        return (
            _receiptHasVoted[proposalId][voter],
            _receiptSupport[proposalId][voter],
            _receiptVotes[proposalId][voter]
        );
    }

    function getProposalCount() external view returns (uint256) {
        return proposalCount;
    }
}
