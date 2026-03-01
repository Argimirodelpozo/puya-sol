// SPDX-License-Identifier: MIT
// Weighted Voting system with quorum for Algorand AVM via puya-sol.
pragma solidity ^0.8.20;

abstract contract Voting2 {
    address private _admin;
    uint256 private _pollCount;
    uint256 private _voterCount;
    uint256 private _totalVotesCast;

    mapping(uint256 => uint256) internal _pollQuorum;
    mapping(uint256 => uint256) internal _pollYesVotes;
    mapping(uint256 => uint256) internal _pollNoVotes;
    mapping(uint256 => bool) internal _pollFinalized;
    mapping(uint256 => bool) internal _pollPassed;
    mapping(address => uint256) internal _voterWeight;
    mapping(address => uint256) internal _voterIndex;

    constructor() {
        _admin = msg.sender;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function pollCount() external view returns (uint256) {
        return _pollCount;
    }

    function voterCount() external view returns (uint256) {
        return _voterCount;
    }

    function totalVotesCast() external view returns (uint256) {
        return _totalVotesCast;
    }

    function pollQuorum(uint256 pollId) external view returns (uint256) {
        return _pollQuorum[pollId];
    }

    function pollYesVotes(uint256 pollId) external view returns (uint256) {
        return _pollYesVotes[pollId];
    }

    function pollNoVotes(uint256 pollId) external view returns (uint256) {
        return _pollNoVotes[pollId];
    }

    function pollFinalized(uint256 pollId) external view returns (bool) {
        return _pollFinalized[pollId];
    }

    function pollPassed(uint256 pollId) external view returns (bool) {
        return _pollPassed[pollId];
    }

    function voterWeight(address voter) external view returns (uint256) {
        return _voterWeight[voter];
    }

    function voterIndex(address voter) external view returns (uint256) {
        return _voterIndex[voter];
    }

    function registerVoter(address voter, uint256 weight) external {
        require(msg.sender == _admin, "only admin");
        require(weight > 0, "weight must be positive");
        require(_voterIndex[voter] == 0, "already registered");
        _voterCount = _voterCount + 1;
        _voterIndex[voter] = _voterCount;
        _voterWeight[voter] = weight;
    }

    function createPoll(uint256 quorum) external returns (uint256) {
        require(msg.sender == _admin, "only admin");
        require(quorum > 0, "quorum must be positive");
        uint256 id = _pollCount;
        _pollCount = id + 1;
        _pollQuorum[id] = quorum;
        _pollYesVotes[id] = 0;
        _pollNoVotes[id] = 0;
        _pollFinalized[id] = false;
        _pollPassed[id] = false;
        return id;
    }

    function vote(uint256 pollId, address voter, bool support) external {
        require(pollId < _pollCount, "poll does not exist");
        require(!_pollFinalized[pollId], "poll already finalized");
        require(_voterWeight[voter] > 0, "voter not registered");
        uint256 weight = _voterWeight[voter];
        if (support) {
            _pollYesVotes[pollId] = _pollYesVotes[pollId] + weight;
        } else {
            _pollNoVotes[pollId] = _pollNoVotes[pollId] + weight;
        }
        _totalVotesCast = _totalVotesCast + 1;
    }

    function finalizePoll(uint256 pollId) external {
        require(pollId < _pollCount, "poll does not exist");
        require(!_pollFinalized[pollId], "poll already finalized");
        uint256 totalVotes = _pollYesVotes[pollId] + _pollNoVotes[pollId];
        require(totalVotes >= _pollQuorum[pollId], "quorum not met");
        _pollFinalized[pollId] = true;
        _pollPassed[pollId] = _pollYesVotes[pollId] > _pollNoVotes[pollId];
    }

    function getPollResult(uint256 pollId) external view returns (bool) {
        require(_pollFinalized[pollId], "poll not finalized");
        return _pollPassed[pollId];
    }
}

contract Voting2Test is Voting2 {
    constructor() Voting2() {}

    function initVoter(address voter) external {
        _voterWeight[voter] = 0;
        _voterIndex[voter] = 0;
    }

    function initPoll(uint256 pollId) external {
        _pollQuorum[pollId] = 0;
        _pollYesVotes[pollId] = 0;
        _pollNoVotes[pollId] = 0;
        _pollFinalized[pollId] = false;
        _pollPassed[pollId] = false;
    }
}
