// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/MerkleTreeLib.sol";

contract MerkleTreeWrapper {
    function build(bytes32[] calldata leaves) external pure returns (bytes32[] memory) {
        bytes32[] memory leavesM = leaves;
        return MerkleTreeLib.build(leavesM);
    }
}
