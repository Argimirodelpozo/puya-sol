// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract BlockSeed {
    function seed() external view returns (uint256, uint256) {
        return (block.prevrandao, block.difficulty);
    }
}
