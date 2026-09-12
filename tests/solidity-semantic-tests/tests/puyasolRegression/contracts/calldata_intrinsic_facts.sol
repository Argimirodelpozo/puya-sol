// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract CalldataIntrinsicFacts {
    uint256 private constructorLength;
    bytes4 private constructorSelector;

    constructor(uint256) {
        constructorLength = msg.data.length;
        constructorSelector = msg.sig;
    }

    function construction() external view returns (uint256, bytes4) {
        return (constructorLength, constructorSelector);
    }

    function inspect(uint16, bool) external pure returns (bytes memory, bytes4) {
        return (msg.data, msg.sig);
    }

    fallback(bytes calldata input) external returns (bytes memory) {
        return abi.encode(input, msg.data, msg.sig);
    }
}

contract BlockSeedFacts {
    function seed() external view returns (uint256, uint256) {
        return (block.prevrandao, block.difficulty);
    }

    function assemblySeed() external view returns (uint256 result) {
        assembly { result := prevrandao() }
    }
}
