// SPDX-License-Identifier: MIT
// Ranked-Choice / Multi-Option Voting system for Algorand AVM via puya-sol.
pragma solidity ^0.8.20;

abstract contract Voting3 {
    address private _admin;
    uint256 private _electionCount;
    uint256 private _totalVotesCast;

    mapping(uint256 => uint256) internal _electionOptions;
    mapping(uint256 => uint256) internal _electionVotesOpt0;
    mapping(uint256 => uint256) internal _electionVotesOpt1;
    mapping(uint256 => uint256) internal _electionVotesOpt2;
    mapping(uint256 => uint256) internal _electionVotesOpt3;
    mapping(uint256 => bool) internal _electionClosed;
    mapping(uint256 => uint256) internal _electionTotalVotes;

    constructor() {
        _admin = msg.sender;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function electionCount() external view returns (uint256) {
        return _electionCount;
    }

    function totalVotesCast() external view returns (uint256) {
        return _totalVotesCast;
    }

    function electionOptions(uint256 electionId) external view returns (uint256) {
        return _electionOptions[electionId];
    }

    function electionClosed(uint256 electionId) external view returns (bool) {
        return _electionClosed[electionId];
    }

    function electionTotalVotes(uint256 electionId) external view returns (uint256) {
        return _electionTotalVotes[electionId];
    }

    function createElection(uint256 numOptions) external returns (uint256) {
        require(msg.sender == _admin, "only admin");
        require(numOptions >= 2, "min 2 options");
        require(numOptions <= 4, "max 4 options");
        uint256 id = _electionCount;
        _electionCount = id + 1;
        _electionOptions[id] = numOptions;
        _electionVotesOpt0[id] = 0;
        _electionVotesOpt1[id] = 0;
        _electionVotesOpt2[id] = 0;
        _electionVotesOpt3[id] = 0;
        _electionClosed[id] = false;
        _electionTotalVotes[id] = 0;
        return id;
    }

    function castVote(uint256 electionId, uint256 option) external {
        require(electionId < _electionCount, "election does not exist");
        require(!_electionClosed[electionId], "election closed");
        require(option < _electionOptions[electionId], "invalid option");
        if (option == 0) {
            _electionVotesOpt0[electionId] = _electionVotesOpt0[electionId] + 1;
        } else if (option == 1) {
            _electionVotesOpt1[electionId] = _electionVotesOpt1[electionId] + 1;
        } else if (option == 2) {
            _electionVotesOpt2[electionId] = _electionVotesOpt2[electionId] + 1;
        } else {
            _electionVotesOpt3[electionId] = _electionVotesOpt3[electionId] + 1;
        }
        _electionTotalVotes[electionId] = _electionTotalVotes[electionId] + 1;
        _totalVotesCast = _totalVotesCast + 1;
    }

    function closeElection(uint256 electionId) external {
        require(msg.sender == _admin, "only admin");
        require(electionId < _electionCount, "election does not exist");
        require(!_electionClosed[electionId], "already closed");
        _electionClosed[electionId] = true;
    }

    function getVotesForOption(uint256 electionId, uint256 option) external view returns (uint256) {
        require(electionId < _electionCount, "election does not exist");
        require(option < _electionOptions[electionId], "invalid option");
        if (option == 0) {
            return _electionVotesOpt0[electionId];
        } else if (option == 1) {
            return _electionVotesOpt1[electionId];
        } else if (option == 2) {
            return _electionVotesOpt2[electionId];
        } else {
            return _electionVotesOpt3[electionId];
        }
    }

    function getWinningOption(uint256 electionId) external view returns (uint256) {
        require(electionId < _electionCount, "election does not exist");
        uint256 numOpts = _electionOptions[electionId];
        uint256 bestOption = 0;
        uint256 bestVotes = _electionVotesOpt0[electionId];
        uint256 votes1 = _electionVotesOpt1[electionId];
        if (votes1 > bestVotes) {
            bestOption = 1;
            bestVotes = votes1;
        }
        if (numOpts > 2) {
            uint256 votes2 = _electionVotesOpt2[electionId];
            if (votes2 > bestVotes) {
                bestOption = 2;
                bestVotes = votes2;
            }
        }
        if (numOpts > 3) {
            uint256 votes3 = _electionVotesOpt3[electionId];
            if (votes3 > bestVotes) {
                bestOption = 3;
                bestVotes = votes3;
            }
        }
        return bestOption;
    }
}

contract Voting3Test is Voting3 {
    constructor() Voting3() {}

    function initElection(uint256 electionId) external {
        _electionOptions[electionId] = 0;
        _electionVotesOpt0[electionId] = 0;
        _electionVotesOpt1[electionId] = 0;
        _electionVotesOpt2[electionId] = 0;
        _electionVotesOpt3[electionId] = 0;
        _electionClosed[electionId] = false;
        _electionTotalVotes[electionId] = 0;
    }
}
