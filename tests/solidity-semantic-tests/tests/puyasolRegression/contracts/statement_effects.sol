// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract StatementEffects {
    function next(uint64[] memory state, uint64 n) internal pure returns (uint64) {
        ++state[0];
        return n;
    }

    function bytesAllocation(uint64 n) external pure returns (uint256 length, uint256, uint64) {
        uint64[] memory state = new uint64[](1);
        bytes memory value = new bytes(next(state, n));
        assembly { length := mload(value) }
        return (length, value.length, state[0]);
    }

    function stringAllocation(uint64 n) external pure returns (uint256 length, uint256, uint64) {
        uint64[] memory state = new uint64[](1);
        string memory value = new string(next(state, n));
        assembly { length := mload(value) }
        return (length, bytes(value).length, state[0]);
    }

    function makeArray(uint64[] memory state) internal pure returns (uint64[] memory value, uint256) {
        ++state[0];
        value = new uint64[](2);
        value[0] = 17;
        return (value, 19);
    }

    function tupleAllocation() external pure returns (uint256 length, uint256 head, uint256, uint64) {
        uint64[] memory state = new uint64[](1);
        (uint64[] memory value, uint256 marker) = makeArray(state);
        assembly { length := mload(value) head := mload(add(value, 32)) }
        return (length, head, marker, state[0]);
    }

    function touch(uint64[] memory state) internal pure { ++state[0]; }
    function leave(uint64[] memory state, bool early) internal pure {
        if (early) return;
        return touch(state);
    }
    function voidEffect(bool early) external pure returns (uint64) {
        uint64[] memory state = new uint64[](1);
        leave(state, early);
        return state[0];
    }
}
