// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// @title Type conversion regression test.
contract TypeConversions {
    // uint256 <-> uint64
    function uint256ToUint64(uint256 x) external pure returns (uint64) {
        return uint64(x);
    }

    function uint64ToUint256(uint64 x) external pure returns (uint256) {
        return uint256(x);
    }

    // uint256 <-> bytes32
    function uint256ToBytes32(uint256 x) external pure returns (bytes32) {
        return bytes32(x);
    }

    function bytes32ToUint256(bytes32 x) external pure returns (uint256) {
        return uint256(x);
    }

    // uint256 <-> bool
    function boolToUint(bool x) external pure returns (uint256) {
        return x ? 1 : 0;
    }

    // Narrowing cast with mask
    function narrowUint256ToUint160(uint256 x) external pure returns (uint256) {
        return uint256(uint160(x));
    }

    // address <-> uint160
    function addressToUint(address a) external pure returns (uint256) {
        return uint256(uint160(a));
    }

    // Explicit type(uint256).max
    function maxUint256() external pure returns (uint256) {
        return type(uint256).max;
    }

    function maxUint64() external pure returns (uint64) {
        return type(uint64).max;
    }
}
