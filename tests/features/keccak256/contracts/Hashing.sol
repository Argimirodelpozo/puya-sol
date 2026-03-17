// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

contract Hashing {
    function hashUint(uint256 val) external pure returns (bytes32) {
        return keccak256(abi.encode(val));
    }

    function hashPacked(address a, uint256 b) external pure returns (bytes32) {
        return keccak256(abi.encodePacked(a, b));
    }

    function hashBytes(bytes memory data) external pure returns (bytes32) {
        return keccak256(data);
    }

    function hashTwoValues(uint256 a, uint256 b) external pure returns (bytes32) {
        return keccak256(abi.encodePacked(a, b));
    }

    function hashString(string memory s) external pure returns (bytes32) {
        return keccak256(bytes(s));
    }

    // Determinism: same input → same output
    function doubleHash(uint256 val) external pure returns (bytes32) {
        bytes32 h1 = keccak256(abi.encode(val));
        bytes32 h2 = keccak256(abi.encode(val));
        require(h1 == h2, "non-deterministic");
        return h1;
    }

    // SHA256 (native AVM opcode)
    function sha256Hash(bytes memory data) external pure returns (bytes32) {
        return sha256(data);
    }
}
