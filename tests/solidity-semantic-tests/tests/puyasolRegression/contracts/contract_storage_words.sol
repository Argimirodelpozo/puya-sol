pragma solidity ^0.8.20;

contract RawStorageRead {
    uint256 stored;
    function named() external view returns (uint256 value) {
        assembly { value := sload(stored.slot) }
    }
    function constantSlot() external view returns (uint256 value) {
        assembly { value := sload(0) }
    }
    function read(uint256 slot) external view returns (uint256 value) {
        assembly { value := sload(slot) }
    }
    function store(uint256 slot, uint256 value) external returns (uint256) {
        assembly { sstore(slot, value) }
        return value;
    }
}
