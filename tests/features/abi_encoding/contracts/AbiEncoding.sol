// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title ABI encoding regression test.
contract AbiEncoding {
    function encodeUint(uint256 val) external pure returns (bytes memory) {
        return abi.encode(val);
    }

    function encodeTwoUints(uint256 a, uint256 b) external pure returns (bytes memory) {
        return abi.encode(a, b);
    }

    function encodePackedTwoAddresses(address a, address b) external pure returns (bytes memory) {
        return abi.encodePacked(a, b);
    }

    function hashPacked(address a, uint256 b) external pure returns (bytes32) {
        return keccak256(abi.encodePacked(a, b));
    }

    // Multiple return values
    function multiReturn() external pure returns (uint256, bool, address) {
        return (42, true, address(0));
    }

    function tupleReturn() external pure returns (uint256 x, uint256 y) {
        x = 10;
        y = 20;
    }
}
