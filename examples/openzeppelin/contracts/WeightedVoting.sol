// SPDX-License-Identifier: MIT
// Weighted voting contract with delegation and proposal lifecycle
// Tests: complex state machines, nested conditionals, multiple mappings with keccak keys

pragma solidity ^0.8.20;

error VotingNotAuthorized(address caller);
error VotingInvalidProposal(uint256 proposalId);
error VotingAlreadyVoted(address voter, uint256 proposalId);
error VotingProposalNotActive(uint256 proposalId, uint256 state);
error VotingInsufficientWeight(address voter, uint256 weight);
error VotingProposalExpired(uint256 proposalId);
error VotingProposalNotExpired(uint256 proposalId);

contract WeightedVoting {
    enum ProposalState { Pending, Active, Passed, Rejected, Expired }

    event ProposalCreated(uint256 indexed proposalId, address indexed proposer, uint256 endTime);
    event Voted(uint256 indexed proposalId, address indexed voter, bool support, uint256 weight);
    event WeightAssigned(address indexed voter, uint256 weight);
    event ProposalFinalized(uint256 indexed proposalId, uint256 state);
    event DelegateChanged(address indexed delegator, address indexed fromDelegate, address indexed toDelegate);

    address private _admin;

    // Voter weights
    mapping(address => uint256) private _weights;
    mapping(address => address) private _delegates;

    // Proposal data (flat mappings)
    mapping(uint256 => address) private _proposers;
    mapping(uint256 => uint256) private _endTimes;
    mapping(uint256 => uint256) private _states;     // ProposalState as uint256
    mapping(uint256 => uint256) private _forVotes;
    mapping(uint256 => uint256) private _againstVotes;
    mapping(uint256 => uint256) private _quorum;     // minimum total votes needed

    // Vote tracking: keccak256(proposalId, voter) => hasVoted
    mapping(bytes32 => bool) private _hasVoted;

    uint256 private _proposalCount;
    uint256 private _totalWeight;

    constructor() {
        _admin = msg.sender;
    }

    modifier onlyAdmin() {
        if (msg.sender != _admin) {
            revert VotingNotAuthorized(msg.sender);
        }
        _;
    }

    // --- Weight management ---

    function assignWeight(address voter, uint256 weight) public onlyAdmin {
        uint256 oldWeight = _weights[voter];
        _weights[voter] = weight;

        if (oldWeight == 0 && weight > 0) {
            _totalWeight += weight;
        } else if (oldWeight > 0 && weight == 0) {
            _totalWeight -= oldWeight;
        } else {
            _totalWeight = _totalWeight - oldWeight + weight;
        }

        emit WeightAssigned(voter, weight);
    }

    function delegate(address to) public {
        address oldDelegate = _delegates[msg.sender];
        _delegates[msg.sender] = to;
        emit DelegateChanged(msg.sender, oldDelegate, to);
    }

    // --- Proposal lifecycle ---

    function createProposal(uint256 duration, uint256 quorum) public returns (uint256) {
        uint256 weight = _getEffectiveWeight(msg.sender);
        if (weight == 0) {
            revert VotingInsufficientWeight(msg.sender, 0);
        }

        uint256 proposalId = _proposalCount;
        _proposalCount = proposalId + 1;

        _proposers[proposalId] = msg.sender;
        _endTimes[proposalId] = block.timestamp + duration;
        _states[proposalId] = uint256(ProposalState.Active);
        _quorum[proposalId] = quorum;

        emit ProposalCreated(proposalId, msg.sender, block.timestamp + duration);
        return proposalId;
    }

    function vote(uint256 proposalId, bool support) public {
        if (proposalId >= _proposalCount) {
            revert VotingInvalidProposal(proposalId);
        }
        if (_states[proposalId] != uint256(ProposalState.Active)) {
            revert VotingProposalNotActive(proposalId, _states[proposalId]);
        }

        uint256 weight = _getEffectiveWeight(msg.sender);
        if (weight == 0) {
            revert VotingInsufficientWeight(msg.sender, 0);
        }

        bytes32 voteKey = keccak256(abi.encodePacked(proposalId, msg.sender));
        if (_hasVoted[voteKey]) {
            revert VotingAlreadyVoted(msg.sender, proposalId);
        }

        _hasVoted[voteKey] = true;

        if (support) {
            _forVotes[proposalId] += weight;
        } else {
            _againstVotes[proposalId] += weight;
        }

        emit Voted(proposalId, msg.sender, support, weight);
    }

    function finalize(uint256 proposalId) public {
        if (proposalId >= _proposalCount) {
            revert VotingInvalidProposal(proposalId);
        }
        if (_states[proposalId] != uint256(ProposalState.Active)) {
            revert VotingProposalNotActive(proposalId, _states[proposalId]);
        }

        uint256 totalVotes = _forVotes[proposalId] + _againstVotes[proposalId];

        if (block.timestamp > _endTimes[proposalId]) {
            if (totalVotes >= _quorum[proposalId] && _forVotes[proposalId] > _againstVotes[proposalId]) {
                _states[proposalId] = uint256(ProposalState.Passed);
            } else if (totalVotes >= _quorum[proposalId]) {
                _states[proposalId] = uint256(ProposalState.Rejected);
            } else {
                _states[proposalId] = uint256(ProposalState.Expired);
            }
        } else {
            // Can early-finalize if quorum met and result clear
            if (totalVotes >= _quorum[proposalId]) {
                if (_forVotes[proposalId] > _againstVotes[proposalId]) {
                    _states[proposalId] = uint256(ProposalState.Passed);
                } else {
                    _states[proposalId] = uint256(ProposalState.Rejected);
                }
            } else {
                revert VotingProposalNotExpired(proposalId);
            }
        }

        emit ProposalFinalized(proposalId, _states[proposalId]);
    }

    // --- View functions ---

    function getWeight(address voter) public view returns (uint256) {
        return _weights[voter];
    }

    function getDelegate(address voter) public view returns (address) {
        return _delegates[voter];
    }

    function getEffectiveWeight(address voter) public view returns (uint256) {
        return _getEffectiveWeight(voter);
    }

    function getProposalState(uint256 proposalId) public view returns (uint256) {
        return _states[proposalId];
    }

    function getProposalVotes(uint256 proposalId) public view returns (uint256 forVotes, uint256 againstVotes) {
        return (_forVotes[proposalId], _againstVotes[proposalId]);
    }

    function getProposalEndTime(uint256 proposalId) public view returns (uint256) {
        return _endTimes[proposalId];
    }

    function getQuorum(uint256 proposalId) public view returns (uint256) {
        return _quorum[proposalId];
    }

    function hasVoted(uint256 proposalId, address voter) public view returns (bool) {
        bytes32 voteKey = keccak256(abi.encodePacked(proposalId, voter));
        return _hasVoted[voteKey];
    }

    function proposalCount() public view returns (uint256) {
        return _proposalCount;
    }

    function totalWeight() public view returns (uint256) {
        return _totalWeight;
    }

    function admin() public view returns (address) {
        return _admin;
    }

    // --- Internal ---

    function _getEffectiveWeight(address voter) private view returns (uint256) {
        address del = _delegates[voter];
        if (del != address(0)) {
            // Delegated: use delegate's weight
            return _weights[del];
        }
        return _weights[voter];
    }
}
