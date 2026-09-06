// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract OversizedEncodedArray {
    uint256[134217728] internal values;
    function first() external view returns (uint256) { return values[0]; }
}
