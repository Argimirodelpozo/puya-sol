// SPDX-License-Identifier: MIT
// Multi-party escrow contract with arbiter resolution
// Tests: multiple mappings, enum state machine, complex conditionals, events

pragma solidity ^0.8.20;

error EscrowNotParticipant(address caller);
error EscrowInvalidState(uint256 current, uint256 expected);
error EscrowAlreadyVoted(address voter);
error EscrowInsufficientAmount();
error EscrowZeroAddress();
error EscrowDeadlinePassed();
error EscrowDeadlineNotPassed();

contract EscrowMultiParty {
    enum State { Created, Funded, Disputed, Resolved, Refunded }

    event EscrowCreated(uint256 indexed escrowId, address indexed depositor, address indexed beneficiary);
    event EscrowFunded(uint256 indexed escrowId, uint256 amount);
    event EscrowDisputed(uint256 indexed escrowId, address indexed disputant);
    event EscrowResolved(uint256 indexed escrowId, bool releasedToBeneficiary);
    event EscrowRefunded(uint256 indexed escrowId);
    event VoteCast(uint256 indexed escrowId, address indexed voter, bool releaseToBeneficiary);

    // Escrow state stored in flat mappings (no struct-with-mapping)
    mapping(uint256 => address) private _depositors;
    mapping(uint256 => address) private _beneficiaries;
    mapping(uint256 => address) private _arbiters;
    mapping(uint256 => uint256) private _amounts;
    mapping(uint256 => uint256) private _states;       // State enum as uint256
    mapping(uint256 => uint256) private _deadlines;
    mapping(uint256 => uint256) private _votesRelease; // votes to release to beneficiary
    mapping(uint256 => uint256) private _votesRefund;  // votes to refund to depositor

    // Track who has voted: keccak256(escrowId, voter) => bool
    mapping(bytes32 => bool) private _hasVoted;

    uint256 private _nextEscrowId;

    function createEscrow(
        address beneficiary,
        address arbiter,
        uint256 amount,
        uint256 deadline
    ) public returns (uint256) {
        if (beneficiary == address(0)) {
            revert EscrowZeroAddress();
        }
        if (arbiter == address(0)) {
            revert EscrowZeroAddress();
        }
        if (amount == 0) {
            revert EscrowInsufficientAmount();
        }

        uint256 escrowId = _nextEscrowId;
        _nextEscrowId = escrowId + 1;

        _depositors[escrowId] = msg.sender;
        _beneficiaries[escrowId] = beneficiary;
        _arbiters[escrowId] = arbiter;
        _amounts[escrowId] = amount;
        _states[escrowId] = uint256(State.Created);
        _deadlines[escrowId] = deadline;

        emit EscrowCreated(escrowId, msg.sender, beneficiary);
        return escrowId;
    }

    function fund(uint256 escrowId) public {
        _requireState(escrowId, State.Created);
        _requireDepositor(escrowId);

        _states[escrowId] = uint256(State.Funded);
        emit EscrowFunded(escrowId, _amounts[escrowId]);
    }

    function dispute(uint256 escrowId) public {
        _requireState(escrowId, State.Funded);
        _requireParticipant(escrowId);

        _states[escrowId] = uint256(State.Disputed);
        emit EscrowDisputed(escrowId, msg.sender);
    }

    function vote(uint256 escrowId, bool releaseToBeneficiary) public {
        _requireState(escrowId, State.Disputed);
        _requireParticipant(escrowId);

        bytes32 voteKey = keccak256(abi.encodePacked(escrowId, msg.sender));
        if (_hasVoted[voteKey]) {
            revert EscrowAlreadyVoted(msg.sender);
        }

        _hasVoted[voteKey] = true;

        if (releaseToBeneficiary) {
            _votesRelease[escrowId] += 1;
        } else {
            _votesRefund[escrowId] += 1;
        }

        emit VoteCast(escrowId, msg.sender, releaseToBeneficiary);

        // Auto-resolve if 2+ votes in one direction
        if (_votesRelease[escrowId] >= 2) {
            _states[escrowId] = uint256(State.Resolved);
            emit EscrowResolved(escrowId, true);
        } else if (_votesRefund[escrowId] >= 2) {
            _states[escrowId] = uint256(State.Refunded);
            emit EscrowResolved(escrowId, false);
        }
    }

    function refundExpired(uint256 escrowId) public {
        uint256 state = _states[escrowId];
        if (state != uint256(State.Funded) && state != uint256(State.Disputed)) {
            revert EscrowInvalidState(state, uint256(State.Funded));
        }
        _requireDepositor(escrowId);

        // On AVM, block.timestamp is current round timestamp
        if (block.timestamp <= _deadlines[escrowId]) {
            revert EscrowDeadlineNotPassed();
        }

        _states[escrowId] = uint256(State.Refunded);
        emit EscrowRefunded(escrowId);
    }

    // --- View functions ---

    function getDepositor(uint256 escrowId) public view returns (address) {
        return _depositors[escrowId];
    }

    function getBeneficiary(uint256 escrowId) public view returns (address) {
        return _beneficiaries[escrowId];
    }

    function getArbiter(uint256 escrowId) public view returns (address) {
        return _arbiters[escrowId];
    }

    function getAmount(uint256 escrowId) public view returns (uint256) {
        return _amounts[escrowId];
    }

    function getState(uint256 escrowId) public view returns (uint256) {
        return _states[escrowId];
    }

    function getDeadline(uint256 escrowId) public view returns (uint256) {
        return _deadlines[escrowId];
    }

    function getVotes(uint256 escrowId) public view returns (uint256 release, uint256 refund) {
        return (_votesRelease[escrowId], _votesRefund[escrowId]);
    }

    function hasVoted(uint256 escrowId, address voter) public view returns (bool) {
        bytes32 voteKey = keccak256(abi.encodePacked(escrowId, voter));
        return _hasVoted[voteKey];
    }

    function escrowCount() public view returns (uint256) {
        return _nextEscrowId;
    }

    // --- Internal helpers ---

    function _requireState(uint256 escrowId, State expected) private view {
        uint256 current = _states[escrowId];
        if (current != uint256(expected)) {
            revert EscrowInvalidState(current, uint256(expected));
        }
    }

    function _requireDepositor(uint256 escrowId) private view {
        if (msg.sender != _depositors[escrowId]) {
            revert EscrowNotParticipant(msg.sender);
        }
    }

    function _requireParticipant(uint256 escrowId) private view {
        address sender = msg.sender;
        if (
            sender != _depositors[escrowId] &&
            sender != _beneficiaries[escrowId] &&
            sender != _arbiters[escrowId]
        ) {
            revert EscrowNotParticipant(sender);
        }
    }
}
