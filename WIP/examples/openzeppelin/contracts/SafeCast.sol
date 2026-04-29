// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

library SafeCast {
    error SafeCastOverflowedUintDowncast(uint8 bits, uint256 value);
    error SafeCastOverflowedIntDowncast(uint8 bits, int256 value);
    error SafeCastOverflowedIntToUint(int256 value);
    error SafeCastOverflowedUintToInt(uint256 value);

    function toUint248(uint256 value) internal pure returns (uint248) {
        if (value > type(uint248).max) revert SafeCastOverflowedUintDowncast(248, value);
        return uint248(value);
    }

    function toUint128(uint256 value) internal pure returns (uint128) {
        if (value > type(uint128).max) revert SafeCastOverflowedUintDowncast(128, value);
        return uint128(value);
    }

    function toUint96(uint256 value) internal pure returns (uint96) {
        if (value > type(uint96).max) revert SafeCastOverflowedUintDowncast(96, value);
        return uint96(value);
    }

    function toUint64(uint256 value) internal pure returns (uint64) {
        if (value > type(uint64).max) revert SafeCastOverflowedUintDowncast(64, value);
        return uint64(value);
    }

    function toUint32(uint256 value) internal pure returns (uint32) {
        if (value > type(uint32).max) revert SafeCastOverflowedUintDowncast(32, value);
        return uint32(value);
    }

    function toUint16(uint256 value) internal pure returns (uint16) {
        if (value > type(uint16).max) revert SafeCastOverflowedUintDowncast(16, value);
        return uint16(value);
    }

    function toUint8(uint256 value) internal pure returns (uint8) {
        if (value > type(uint8).max) revert SafeCastOverflowedUintDowncast(8, value);
        return uint8(value);
    }
}

contract SafeCastTest {
    function testToUint128(uint256 value) external pure returns (uint128) {
        return SafeCast.toUint128(value);
    }

    function testToUint96(uint256 value) external pure returns (uint96) {
        return SafeCast.toUint96(value);
    }

    function testToUint64(uint256 value) external pure returns (uint64) {
        return SafeCast.toUint64(value);
    }

    function testToUint32(uint256 value) external pure returns (uint32) {
        return SafeCast.toUint32(value);
    }

    function testToUint16(uint256 value) external pure returns (uint16) {
        return SafeCast.toUint16(value);
    }

    function testToUint8(uint256 value) external pure returns (uint8) {
        return SafeCast.toUint8(value);
    }
}
