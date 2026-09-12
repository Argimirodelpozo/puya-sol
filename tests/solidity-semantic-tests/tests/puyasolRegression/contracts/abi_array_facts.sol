// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract AbiArrayFacts {
    struct Item { uint16 n; string text; bool ok; }

    function bytesValue(bytes memory data) external pure returns (bytes memory) {
        return abi.decode(data, (bytes));
    }

    function fixedBits(bytes memory data) external pure returns (bytes memory) {
        return abi.encode(abi.decode(data, (bool[9])));
    }

    function fixedStrings(bytes memory data) external pure returns (bytes memory) {
        return abi.encode(abi.decode(data, (string[2])));
    }

    function nested(bytes memory data) external pure returns (bytes memory) {
        return abi.encode(abi.decode(data, (uint16[][2])));
    }

    function structures(bytes memory data) external pure returns (bytes memory) {
        return abi.encode(abi.decode(data, (Item[2])));
    }

    function words(uint256[] memory data) external pure returns (bytes memory) {
        return abi.encode(data);
    }

    function signedWords(int256[3] memory data) external pure returns (bytes memory) {
        return abi.encode(data);
    }

    function fixedWords(bytes32[3] memory data) external pure returns (bytes memory) {
        return abi.encodePacked(data);
    }

    function routedBits(bool[9] memory data) external pure returns (bytes memory) {
        return abi.encode(data);
    }

    function routedDynamicBits(bool[] memory data) external pure returns (bytes memory) {
        return abi.encode(data);
    }

    function routedStrings(string[2] memory data) external pure returns (bytes memory) {
        return abi.encode(data);
    }

    function routedNested(uint16[][2] memory data) external pure returns (bytes memory) {
        return abi.encode(data);
    }

    function routedStructures(Item[2] memory data) external pure returns (bytes memory) {
        return abi.encode(data);
    }
}
