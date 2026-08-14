pragma solidity ^0.8.20;

contract A {
    uint256 private value;

    function set(uint256 next) external { value = next; }

    function raw() external view returns (uint256 result) {
        assembly { result := sload(value.slot) }
    }
}

contract B {
    uint256 private other;

    function set(uint256 next) external { other = next; }

    function raw() external view returns (uint256 result) {
        assembly { result := sload(other.slot) }
    }
}
