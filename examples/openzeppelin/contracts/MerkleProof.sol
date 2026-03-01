// SPDX-License-Identifier: MIT
// Inspired by OpenZeppelin MerkleProof.sol
// Demonstrates: library, pure functions, keccak256, abi.encodePacked, bytes32 comparison
pragma solidity ^0.8.20;

library MerkleProof {
    function _hashPair(bytes32 a, bytes32 b) internal pure returns (bytes32) {
        if (uint256(a) < uint256(b)) {
            return keccak256(abi.encodePacked(a, b));
        } else {
            return keccak256(abi.encodePacked(b, a));
        }
    }

    function processProofDepth1(bytes32 leaf, bytes32 p0) internal pure returns (bytes32) {
        return _hashPair(leaf, p0);
    }

    function processProofDepth2(bytes32 leaf, bytes32 p0, bytes32 p1) internal pure returns (bytes32) {
        return _hashPair(_hashPair(leaf, p0), p1);
    }

    function processProofDepth3(bytes32 leaf, bytes32 p0, bytes32 p1, bytes32 p2) internal pure returns (bytes32) {
        return _hashPair(_hashPair(_hashPair(leaf, p0), p1), p2);
    }
}

contract MerkleProofTest {
    function hashPair(bytes32 a, bytes32 b) external pure returns (bytes32) {
        return MerkleProof._hashPair(a, b);
    }

    function verifyDepth1(bytes32 root, bytes32 leaf, bytes32 p0) external pure returns (bool) {
        return MerkleProof.processProofDepth1(leaf, p0) == root;
    }

    function verifyDepth2(bytes32 root, bytes32 leaf, bytes32 p0, bytes32 p1) external pure returns (bool) {
        return MerkleProof.processProofDepth2(leaf, p0, p1) == root;
    }

    function verifyDepth3(bytes32 root, bytes32 leaf, bytes32 p0, bytes32 p1, bytes32 p2) external pure returns (bool) {
        return MerkleProof.processProofDepth3(leaf, p0, p1, p2) == root;
    }

    function computeLeafHash(uint256 value) external pure returns (bytes32) {
        return keccak256(abi.encodePacked(value));
    }
}
