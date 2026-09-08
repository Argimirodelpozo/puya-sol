// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// EIP-1967 coordinates are ordinary storage unless adaptation is requested.
contract ProxyAdaptation {
    bytes32 constant ADMIN =
        0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103;

    function write(uint256 value) external {
        assembly { let slot := ADMIN sstore(slot, value) }
    }

    function read() external view returns (uint256 value) {
        assembly { value := sload(ADMIN) }
    }
}
