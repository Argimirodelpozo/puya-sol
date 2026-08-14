pragma solidity ^0.8.20;

library Slots {
    function read(uint256 slot) internal view returns (uint256 value) {
        assembly { value := sload(slot) }
    }
}

function freeRead(uint256 slot) view returns (uint256 value) {
    assembly { value := sload(slot) }
}

contract HostA {
    uint256 private value;

    function set(uint256 next) external { value = next; }
    function readLibrary() external view returns (uint256) { return Slots.read(0); }
    function readFree() external view returns (uint256) { return freeRead(0); }
}

contract HostB {
    uint256 private other;

    function set(uint256 next) external { other = next; }
    function readLibrary() external view returns (uint256) { return Slots.read(0); }
    function readFree() external view returns (uint256) { return freeRead(0); }
}
