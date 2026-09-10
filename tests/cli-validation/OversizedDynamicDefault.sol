// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract OversizedDynamicDefault {
    function run() external pure returns (uint256) {
        string[10000] memory values;
        return bytes(values[0]).length;
    }
}
