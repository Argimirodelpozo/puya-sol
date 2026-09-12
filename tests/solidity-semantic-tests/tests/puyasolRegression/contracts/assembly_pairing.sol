// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract AssemblyPairing {
    function pair(bytes calldata input) external view returns (uint256 result) {
        assembly {
            calldatacopy(128, input.offset, input.length)
            if iszero(staticcall(gas(), 8, 128, input.length, 4096, 32)) { revert(0, 0) }
            result := mload(4096)
        }
    }
}
