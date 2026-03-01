// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simple ballot/voting contract.
 * Chairman grants voting rights, voters can vote on proposals (by index).
 */
contract BallotTest {
    address private _chairman;
    uint256 private _proposalCount;

    mapping(address => bool) private _hasVoted;
    mapping(address => bool) private _canVote;
    mapping(uint256 => uint256) private _proposalVotes;

    constructor() {
        _chairman = msg.sender;
        _proposalCount = 3; // 3 proposals by default
    }

    function chairman() external view returns (address) {
        return _chairman;
    }

    function proposalCount() external view returns (uint256) {
        return _proposalCount;
    }

    function canVote(address voter) external view returns (bool) {
        return _canVote[voter];
    }

    function hasVoted(address voter) external view returns (bool) {
        return _hasVoted[voter];
    }

    function proposalVotes(uint256 proposalId) external view returns (uint256) {
        return _proposalVotes[proposalId];
    }

    function grantVotingRight(address voter) external {
        require(msg.sender == _chairman, "Not chairman");
        require(!_hasVoted[voter], "Already voted");
        _canVote[voter] = true;
    }

    function vote(address voter, uint256 proposalId) external {
        require(_canVote[voter], "No voting right");
        require(!_hasVoted[voter], "Already voted");
        require(proposalId < _proposalCount, "Invalid proposal");

        _hasVoted[voter] = true;
        _proposalVotes[proposalId] += 1;
    }

    function winningProposal() external view returns (uint256 winId, uint256 winVotes) {
        winVotes = 0;
        winId = 0;

        for (uint256 i = 0; i < _proposalCount; i++) {
            uint256 votes = _proposalVotes[i];
            if (votes > winVotes) {
                winVotes = votes;
                winId = i;
            }
        }
    }
}
