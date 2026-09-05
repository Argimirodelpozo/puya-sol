// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract OversizedArrayLength {
    uint8[9223372036854775808] internal values;
    function length() external view returns (uint256) { return values.length; }
}
