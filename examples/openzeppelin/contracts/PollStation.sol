// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Poll/survey system.
 * Admin creates polls, users submit numeric responses.
 * Tracks response count, total value, and computes averages.
 */
abstract contract PollStation {
    address private _admin;
    uint256 private _pollCount;

    mapping(uint256 => uint256) internal _pollResponseCount;
    mapping(uint256 => uint256) internal _pollTotalValue;
    mapping(uint256 => bool) internal _pollOpen;

    constructor() {
        _admin = msg.sender;
        _pollCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getPollCount() external view returns (uint256) {
        return _pollCount;
    }

    function getPollResponseCount(uint256 pollId) external view returns (uint256) {
        return _pollResponseCount[pollId];
    }

    function getPollTotalValue(uint256 pollId) external view returns (uint256) {
        return _pollTotalValue[pollId];
    }

    function getPollAverage(uint256 pollId) external view returns (uint256) {
        require(_pollResponseCount[pollId] > 0, "No responses");
        return _pollTotalValue[pollId] / _pollResponseCount[pollId];
    }

    function isPollOpen(uint256 pollId) external view returns (bool) {
        return _pollOpen[pollId];
    }

    function createPoll() external returns (uint256) {
        require(msg.sender == _admin, "Not admin");
        uint256 id = _pollCount;
        _pollResponseCount[id] = 0;
        _pollTotalValue[id] = 0;
        _pollOpen[id] = true;
        _pollCount = id + 1;
        return id;
    }

    function submitResponse(uint256 pollId, uint256 responseValue) external {
        require(pollId < _pollCount, "Poll does not exist");
        require(_pollOpen[pollId], "Poll is closed");
        _pollResponseCount[pollId] = _pollResponseCount[pollId] + 1;
        _pollTotalValue[pollId] = _pollTotalValue[pollId] + responseValue;
    }

    function closePoll(uint256 pollId) external {
        require(msg.sender == _admin, "Not admin");
        require(pollId < _pollCount, "Poll does not exist");
        require(_pollOpen[pollId], "Poll already closed");
        _pollOpen[pollId] = false;
    }
}

contract PollStationTest is PollStation {
    constructor() PollStation() {}

    function initPoll(uint256 pollId) external {
        _pollResponseCount[pollId] = 0;
        _pollTotalValue[pollId] = 0;
        _pollOpen[pollId] = false;
    }
}
