// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Different private declarations may have the same source name across bases.
// Their declaration identities, canonical slots, and named AVM cells must stay
// distinct when inline assembly crosses the logical/physical storage boundary.
contract LeftStorage {
    uint256 private value;

    function setLeft(uint256 next) public { value = next; }
    function getLeft() public view returns (uint256) { return value; }

    function leftSlot() public pure returns (uint256 slot) {
        assembly { slot := value.slot }
    }

    function rawLeft() public view returns (uint256 result) {
        assembly { result := sload(value.slot) }
    }

    function storeLeft(uint256 next) public {
        assembly { sstore(value.slot, next) }
    }
}

contract RightStorage {
    uint256 private value;

    function setRight(uint256 next) public { value = next; }
    function getRight() public view returns (uint256) { return value; }

    function rightSlot() public pure returns (uint256 slot) {
        assembly { slot := value.slot }
    }

    function rawRight() public view returns (uint256 result) {
        assembly { result := sload(value.slot) }
    }

    function storeRight(uint256 next) public {
        assembly { sstore(value.slot, next) }
    }
}

contract C is LeftStorage, RightStorage {}
