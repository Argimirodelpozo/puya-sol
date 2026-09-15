// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract SharingRawYul {
    struct Pair { uint256 first; uint256 observed; }

    function inspectFirstWord() internal pure returns (uint256 result) {
        assembly { result := mload(0x80) }
    }
    function constructorOrder() external pure returns (uint256, uint256, uint256 pointer) {
        Pair memory pair = Pair(71, inspectFirstWord());
        assembly { pointer := pair }
        return (pair.first, pair.observed, pointer);
    }

    function readUnnamedObject() external pure returns (uint256 result) {
        uint256[2] memory values;
        values[0] = 61;
        assembly { result := mload(0x80) }
    }
    function writeUnnamedObject() external pure returns (uint256) {
        uint256[2] memory values;
        values[0] = 1;
        assembly { mstore(0x80, 67) }
        return values[0];
    }
}
