// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Multi-round lottery system for Algorand AVM via puya-sol.
 * Admin creates rounds with ticket price and max tickets.
 * Players buy tickets within a round.
 * Admin draws winner by specifying the winning ticket index.
 * Prize pool = ticketPrice * ticketsSold for each round.
 */
abstract contract Lottery2 {
    address private _admin;
    uint256 private _roundCount;
    uint256 private _totalTicketsSold;
    uint256 private _totalPrizesPaid;

    mapping(uint256 => uint256) internal _roundTicketPrice;
    mapping(uint256 => uint256) internal _roundMaxTickets;
    mapping(uint256 => uint256) internal _roundTicketsSold;
    mapping(uint256 => bool) internal _roundDrawn;
    mapping(uint256 => uint256) internal _roundWinnerIndex;
    mapping(uint256 => uint256) internal _roundPrize;

    constructor() {
        _admin = msg.sender;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function roundCount() external view returns (uint256) {
        return _roundCount;
    }

    function totalTicketsSold() external view returns (uint256) {
        return _totalTicketsSold;
    }

    function totalPrizesPaid() external view returns (uint256) {
        return _totalPrizesPaid;
    }

    function getRoundTicketPrice(uint256 roundId) external view returns (uint256) {
        return _roundTicketPrice[roundId];
    }

    function getRoundMaxTickets(uint256 roundId) external view returns (uint256) {
        return _roundMaxTickets[roundId];
    }

    function getRoundTicketsSold(uint256 roundId) external view returns (uint256) {
        return _roundTicketsSold[roundId];
    }

    function isRoundDrawn(uint256 roundId) external view returns (bool) {
        return _roundDrawn[roundId];
    }

    function getRoundWinnerIndex(uint256 roundId) external view returns (uint256) {
        return _roundWinnerIndex[roundId];
    }

    function getRoundPrize(uint256 roundId) external view returns (uint256) {
        return _roundPrize[roundId];
    }

    function createRound(uint256 ticketPrice, uint256 maxTickets) external returns (uint256) {
        uint256 roundId = _roundCount;
        _roundTicketPrice[roundId] = ticketPrice;
        _roundMaxTickets[roundId] = maxTickets;
        _roundTicketsSold[roundId] = 0;
        _roundDrawn[roundId] = false;
        _roundWinnerIndex[roundId] = 0;
        _roundPrize[roundId] = 0;
        _roundCount = roundId + 1;
        return roundId;
    }

    function buyTicket(uint256 roundId) external returns (uint256) {
        require(!_roundDrawn[roundId], "Round already drawn");
        require(_roundTicketsSold[roundId] < _roundMaxTickets[roundId], "No tickets available");

        uint256 ticketIndex = _roundTicketsSold[roundId];
        _roundTicketsSold[roundId] = ticketIndex + 1;
        _roundPrize[roundId] = _roundPrize[roundId] + _roundTicketPrice[roundId];
        _totalTicketsSold = _totalTicketsSold + 1;
        return ticketIndex;
    }

    function drawWinner(uint256 roundId, uint256 winnerIndex) external {
        require(!_roundDrawn[roundId], "Round already drawn");
        require(_roundTicketsSold[roundId] > 0, "No tickets sold");
        require(winnerIndex < _roundTicketsSold[roundId], "Invalid winner index");

        _roundDrawn[roundId] = true;
        _roundWinnerIndex[roundId] = winnerIndex;
        _totalPrizesPaid = _totalPrizesPaid + _roundPrize[roundId];
    }
}

contract Lottery2Test is Lottery2 {
    constructor() Lottery2() {}

    function initRound(uint256 roundId) external {
        _roundTicketPrice[roundId] = 0;
        _roundMaxTickets[roundId] = 0;
        _roundTicketsSold[roundId] = 0;
        _roundDrawn[roundId] = false;
        _roundWinnerIndex[roundId] = 0;
        _roundPrize[roundId] = 0;
    }
}
