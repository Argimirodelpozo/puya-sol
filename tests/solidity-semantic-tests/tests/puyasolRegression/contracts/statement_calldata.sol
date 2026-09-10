// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StatementCalldata {
    function next(uint64[] memory state) internal pure returns (uint256) {
        ++state[0];
        return 1;
    }
    function slice(uint256[2][] calldata input) external pure returns (uint256 offset, uint256 value, uint64) {
        assembly { input.offset := input.offset }
        uint64[] memory state = new uint64[](1);
        uint256[2] calldata row = input[next(state)];
        assembly { offset := sub(row, input.offset) value := calldataload(row) }
        return (offset, value, state[0]);
    }
}
