// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/DateTimeLib.sol";

contract DateTimeWrapper {
    function dateToEpochDay(uint256 year, uint256 month, uint256 day) external pure returns (uint256) {
        return DateTimeLib.dateToEpochDay(year, month, day);
    }

    function epochDayToDate(uint256 epochDay) external pure returns (uint256 year, uint256 month, uint256 day) {
        return DateTimeLib.epochDayToDate(epochDay);
    }

    function dateToTimestamp(uint256 year, uint256 month, uint256 day) external pure returns (uint256) {
        return DateTimeLib.dateToTimestamp(year, month, day);
    }

    function timestampToDate(uint256 timestamp) external pure returns (uint256 year, uint256 month, uint256 day) {
        return DateTimeLib.timestampToDate(timestamp);
    }

    function isLeapYear(uint256 year) external pure returns (bool) {
        return DateTimeLib.isLeapYear(year);
    }

    function daysInMonth(uint256 year, uint256 month) external pure returns (uint256) {
        return DateTimeLib.daysInMonth(year, month);
    }

    function weekday(uint256 timestamp) external pure returns (uint256) {
        return DateTimeLib.weekday(timestamp);
    }
}
