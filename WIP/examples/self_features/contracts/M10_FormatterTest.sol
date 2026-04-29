// SPDX-License-Identifier: MIT
pragma solidity 0.8.28;

import "libraries/Formatter.sol";

/**
 * @title FormatterTest
 * @notice Test contract that wraps Formatter library functions for puya-sol compilation testing.
 */
contract FormatterTest {
    /// @notice Test numAsciiToUint: ASCII code → digit value
    function testNumAsciiToUint(uint256 numAscii) external pure returns (uint256) {
        return Formatter.numAsciiToUint(numAscii);
    }

    /// @notice Test parseDatePart: numeric string → uint
    function testParseDatePart(string memory value) external pure returns (uint256) {
        return Formatter.parseDatePart(value);
    }

    /// @notice Test substring extraction
    function testSubstring(string memory str, uint256 start, uint256 end) external pure returns (string memory) {
        return Formatter.substring(str, start, end);
    }

    /// @notice Test isLeapYear
    function testIsLeapYear(uint256 year) external pure returns (bool) {
        return Formatter.isLeapYear(year);
    }

    /// @notice Test toTimestamp: (year, month, day) → Unix timestamp
    function testToTimestamp(uint256 year, uint256 month, uint256 day) external pure returns (uint256) {
        return Formatter.toTimestamp(year, month, day);
    }

    /// @notice Test formatDate: YYMMDD → "DD-MM-YY"
    function testFormatDate(string memory date) external pure returns (string memory) {
        return Formatter.formatDate(date);
    }
}
