// SPDX-License-Identifier: MIT
pragma solidity ^0.7.0;

import "../MerkleTreeWithHistory.sol";

/// @dev A simple mock hasher that returns keccak256-based values
/// instead of real MiMC. Used only for testing the Merkle tree structure.
/// Output is reduced mod FIELD_SIZE so values stay within the BN254 field.
contract HasherMock is IHasher {
    uint256 constant FIELD_SIZE = 21888242871839275222246405745257275088548364400416034343698204186575808495617;

    function MiMCSponge(uint256 in_xL, uint256 in_xR) external pure override returns (uint256 xL, uint256 xR) {
        bytes32 h = keccak256(abi.encodePacked(in_xL, in_xR));
        xL = uint256(h) % FIELD_SIZE;
        xR = uint256(keccak256(abi.encodePacked(h))) % FIELD_SIZE;
    }
}
