// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract VotingToken {
    address private _admin;
    uint256 private _totalSupply;
    uint256 private _proposalCount;
    uint256 private _votingPeriod;

    mapping(address => uint256) private _balances;
    mapping(uint256 => uint256) private _propForVotes;
    mapping(uint256 => uint256) private _propAgainstVotes;
    mapping(uint256 => uint256) private _propStartTime;
    mapping(uint256 => uint256) private _propEndTime;
    mapping(uint256 => bool) private _propExecuted;

    constructor(uint256 votingPeriod_) {
        _admin = msg.sender;
        _votingPeriod = votingPeriod_;
    }

    function mint(address to, uint256 amount) public {
        require(msg.sender == _admin, "VotingToken: caller is not admin");
        _balances[to] += amount;
        _totalSupply += amount;
    }

    function burn(address from, uint256 amount) public {
        require(msg.sender == _admin, "VotingToken: caller is not admin");
        require(_balances[from] >= amount, "VotingToken: burn amount exceeds balance");
        _balances[from] -= amount;
        _totalSupply -= amount;
    }

    function balanceOf(address who) public view returns (uint256) {
        return _balances[who];
    }

    function getTotalSupply() public view returns (uint256) {
        return _totalSupply;
    }

    function createProposal(uint256 startTime) public returns (uint256) {
        require(msg.sender == _admin, "VotingToken: caller is not admin");
        uint256 proposalId = _proposalCount;
        _proposalCount += 1;
        _propStartTime[proposalId] = startTime;
        _propEndTime[proposalId] = startTime + _votingPeriod;
        return proposalId;
    }

    function voteFor(uint256 proposalId, address voter) public {
        uint256 weight = _balances[voter];
        _propForVotes[proposalId] += weight;
    }

    function voteAgainst(uint256 proposalId, address voter) public {
        uint256 weight = _balances[voter];
        _propAgainstVotes[proposalId] += weight;
    }

    function getForVotes(uint256 proposalId) public view returns (uint256) {
        return _propForVotes[proposalId];
    }

    function getAgainstVotes(uint256 proposalId) public view returns (uint256) {
        return _propAgainstVotes[proposalId];
    }

    function isProposalPassed(uint256 proposalId) public view returns (bool) {
        return _propForVotes[proposalId] > _propAgainstVotes[proposalId];
    }

    function executeProposal(uint256 proposalId) public {
        require(!_propExecuted[proposalId], "VotingToken: proposal already executed");
        require(_propForVotes[proposalId] > _propAgainstVotes[proposalId], "VotingToken: proposal not passed");
        _propExecuted[proposalId] = true;
    }

    function isExecuted(uint256 proposalId) public view returns (bool) {
        return _propExecuted[proposalId];
    }

    function getAdmin() public view returns (address) {
        return _admin;
    }
}

contract VotingTokenTest is VotingToken {
    constructor() VotingToken(100) {}
}
