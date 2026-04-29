// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Event ticket sales and validation system.
 * Admin creates events with a capacity, users buy tickets,
 * and tickets can be marked as used. Tracks ticket and event counts.
 */
abstract contract EventTicketing {
    address private _admin;
    uint256 private _ticketCount;
    uint256 private _eventCount;

    mapping(uint256 => uint256) internal _ticketEvent;
    mapping(uint256 => bool) internal _ticketUsed;
    mapping(uint256 => uint256) internal _eventCapacity;

    constructor() {
        _admin = msg.sender;
        _ticketCount = 0;
        _eventCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getTicketCount() external view returns (uint256) {
        return _ticketCount;
    }

    function getEventCount() external view returns (uint256) {
        return _eventCount;
    }

    function getTicketEvent(uint256 ticketId) external view returns (uint256) {
        return _ticketEvent[ticketId];
    }

    function isTicketUsed(uint256 ticketId) external view returns (bool) {
        return _ticketUsed[ticketId];
    }

    function getEventCapacity(uint256 eventId) external view returns (uint256) {
        return _eventCapacity[eventId];
    }

    function createEvent(uint256 capacity) external returns (uint256) {
        require(msg.sender == _admin, "Only admin");
        require(capacity > 0, "Capacity must be positive");
        uint256 id = _eventCount;
        _eventCapacity[id] = capacity;
        _eventCount = id + 1;
        return id;
    }

    function buyTicket(uint256 eventId) external returns (uint256) {
        require(eventId < _eventCount, "Event does not exist");
        require(_eventCapacity[eventId] > 0, "Event sold out");
        uint256 id = _ticketCount;
        _ticketEvent[id] = eventId;
        _ticketUsed[id] = false;
        _eventCapacity[eventId] = _eventCapacity[eventId] - 1;
        _ticketCount = id + 1;
        return id;
    }

    function useTicket(uint256 ticketId) external {
        require(ticketId < _ticketCount, "Ticket does not exist");
        require(!_ticketUsed[ticketId], "Ticket already used");
        _ticketUsed[ticketId] = true;
    }
}

contract EventTicketingTest is EventTicketing {
    constructor() EventTicketing() {}

    function initTicket(uint256 ticketId) external {
        _ticketEvent[ticketId] = 0;
        _ticketUsed[ticketId] = false;
    }

    function initEvent(uint256 eventId) external {
        _eventCapacity[eventId] = 0;
    }
}
