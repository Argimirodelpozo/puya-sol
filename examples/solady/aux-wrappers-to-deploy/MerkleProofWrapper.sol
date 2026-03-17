// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/MerkleProofLib.sol";

contract MerkleProofWrapper {
    function verify(bytes32[] calldata proof, bytes32 root, bytes32 leaf) external pure returns (bool) {
        return MerkleProofLib.verify(proof, root, leaf);
    }

    function verifyCalldata(bytes32[] calldata proof, bytes32 root, bytes32 leaf) external pure returns (bool) {
        return MerkleProofLib.verifyCalldata(proof, root, leaf);
    }
}
