// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Event scheduling system.
 * Events are scheduled with a timestamp and can be cancelled or completed by admin.
 */
abstract contract Scheduler {
    address private _admin;
    uint256 private _eventCount;
    uint256 private _completedCount;

    mapping(uint256 => uint256) internal _eventTimestamp;
    mapping(uint256 => bool) internal _eventCancelled;
    mapping(uint256 => bool) internal _eventCompleted;

    constructor() {
        _admin = msg.sender;
        _eventCount = 0;
        _completedCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getEventCount() external view returns (uint256) {
        return _eventCount;
    }

    function getCompletedCount() external view returns (uint256) {
        return _completedCount;
    }

    function getEventTimestamp(uint256 eventId) external view returns (uint256) {
        return _eventTimestamp[eventId];
    }

    function isEventScheduled(uint256 eventId) external view returns (bool) {
        if (eventId >= _eventCount) {
            return false;
        }
        if (_eventCancelled[eventId] == true) {
            return false;
        }
        if (_eventCompleted[eventId] == true) {
            return false;
        }
        return true;
    }

    function isEventCompleted(uint256 eventId) external view returns (bool) {
        return _eventCompleted[eventId];
    }

    function scheduleEvent(uint256 eventTimestamp) external returns (uint256) {
        uint256 id = _eventCount;
        _eventTimestamp[id] = eventTimestamp;
        _eventCancelled[id] = false;
        _eventCompleted[id] = false;
        _eventCount = id + 1;
        return id;
    }

    function cancelEvent(uint256 eventId) external {
        require(msg.sender == _admin, "Not admin");
        require(eventId < _eventCount, "Event does not exist");
        require(_eventCancelled[eventId] == false, "Already cancelled");
        require(_eventCompleted[eventId] == false, "Already completed");
        _eventCancelled[eventId] = true;
    }

    function completeEvent(uint256 eventId) external {
        require(msg.sender == _admin, "Not admin");
        require(eventId < _eventCount, "Event does not exist");
        require(_eventCancelled[eventId] == false, "Event is cancelled");
        require(_eventCompleted[eventId] == false, "Already completed");
        _eventCompleted[eventId] = true;
        _completedCount = _completedCount + 1;
    }
}

contract SchedulerTest is Scheduler {
    constructor() Scheduler() {}

    function initEvent(uint256 eventId) external {
        _eventTimestamp[eventId] = 0;
        _eventCancelled[eventId] = false;
        _eventCompleted[eventId] = false;
    }
}
