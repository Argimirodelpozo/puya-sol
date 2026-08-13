pragma solidity ^0.8.0;

contract EvmLayoutBoolNestedArray {
    bool[2][] private flags;
    bool[9][] private wideFlags;

    function set(bool[2][] memory value) external {
        flags = value;
    }

    function get() external view returns (bool[2][] memory) {
        return flags;
    }

    function clear() external {
        delete flags;
    }

    function rawElementWord(uint256 index) external view returns (uint256 word) {
        assembly {
            mstore(0, flags.slot)
            word := sload(add(keccak256(0, 32), index))
        }
    }

    function setWide(bool[9][] memory value) external {
        wideFlags = value;
    }

    function getWide() external view returns (bool[9][] memory) {
        return wideFlags;
    }

    function rawWideElementWord(uint256 index) external view returns (uint256 word) {
        assembly {
            mstore(0, wideFlags.slot)
            word := sload(add(keccak256(0, 32), index))
        }
    }
}
