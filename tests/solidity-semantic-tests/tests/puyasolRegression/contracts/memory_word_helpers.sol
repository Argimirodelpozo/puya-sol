pragma solidity ^0.8.24;

contract MemoryWordHelpers {
    function roundTrip(uint256 p, uint256 value)
        external pure returns (uint256 first, uint256 second, uint256 edges)
    {
        assembly {
            mstore8(sub(p, 1), 0xa5)
            mstore8(add(p, 32), 0x5a)
            mstore(p, value)
            first := mload(p)
            mstore(p, not(value))
            second := mload(p)
            edges := or(shl(8, byte(0, mload(sub(p, 1)))), byte(0, mload(add(p, 32))))
        }
    }

    function word(uint256 p, uint256 value) external pure returns (uint256 result) {
        assembly { mstore(p, value) result := mload(p) }
    }

    function read(uint256 p) external pure returns (uint256 result) {
        assembly { result := mload(p) }
    }
}
