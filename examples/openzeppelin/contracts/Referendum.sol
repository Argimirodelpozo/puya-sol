// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Referendum/proposal voting system on the Algorand AVM.
 * Admin creates proposals, anyone can vote for or against,
 * and admin can close proposals. A proposal passes if for > against.
 */
abstract contract Referendum {
    address private _admin;
    uint256 private _proposalCount;

    mapping(uint256 => uint256) internal _proposalForVotes;
    mapping(uint256 => uint256) internal _proposalAgainstVotes;
    mapping(uint256 => bool) internal _proposalOpen;

    constructor() {
        _admin = msg.sender;
        _proposalCount = 0;
    }

    function getAdmin() public view returns (address) {
        return _admin;
    }

    function getProposalCount() public view returns (uint256) {
        return _proposalCount;
    }

    function getForVotes(uint256 proposalId) public view returns (uint256) {
        return _proposalForVotes[proposalId];
    }

    function getAgainstVotes(uint256 proposalId) public view returns (uint256) {
        return _proposalAgainstVotes[proposalId];
    }

    function isProposalOpen(uint256 proposalId) public view returns (bool) {
        return _proposalOpen[proposalId];
    }

    function getTotalParticipation(uint256 proposalId) public view returns (uint256) {
        uint256 forCount = _proposalForVotes[proposalId];
        uint256 againstCount = _proposalAgainstVotes[proposalId];
        return forCount + againstCount;
    }

    function isProposalPassed(uint256 proposalId) public view returns (bool) {
        require(proposalId < _proposalCount, "Referendum: proposal does not exist");
        uint256 forCount = _proposalForVotes[proposalId];
        uint256 againstCount = _proposalAgainstVotes[proposalId];
        return forCount > againstCount;
    }

    function createProposal() public returns (uint256) {
        require(msg.sender == _admin, "Referendum: not admin");
        uint256 proposalId = _proposalCount;
        _proposalForVotes[proposalId] = 0;
        _proposalAgainstVotes[proposalId] = 0;
        _proposalOpen[proposalId] = true;
        _proposalCount = proposalId + 1;
        return proposalId;
    }

    function voteFor(uint256 proposalId) public {
        require(proposalId < _proposalCount, "Referendum: proposal does not exist");
        require(_proposalOpen[proposalId], "Referendum: proposal is closed");
        _proposalForVotes[proposalId] = _proposalForVotes[proposalId] + 1;
    }

    function voteAgainst(uint256 proposalId) public {
        require(proposalId < _proposalCount, "Referendum: proposal does not exist");
        require(_proposalOpen[proposalId], "Referendum: proposal is closed");
        _proposalAgainstVotes[proposalId] = _proposalAgainstVotes[proposalId] + 1;
    }

    function closeProposal(uint256 proposalId) public {
        require(msg.sender == _admin, "Referendum: not admin");
        require(proposalId < _proposalCount, "Referendum: proposal does not exist");
        require(_proposalOpen[proposalId], "Referendum: proposal already closed");
        _proposalOpen[proposalId] = false;
    }
}

contract ReferendumTest is Referendum {
    constructor() Referendum() {}

    function initProposal(uint256 proposalId) public {
        _proposalForVotes[proposalId] = 0;
        _proposalAgainstVotes[proposalId] = 0;
        _proposalOpen[proposalId] = false;
    }
}
