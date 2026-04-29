// SPDX-License-Identifier: MIT
// Based on OpenZeppelin Contracts v5.0.0 (utils/ShortStrings.sol)
// MODIFIED — inline assembly replaced with pure Solidity equivalents
// Uses uint256 arithmetic instead of bytes32 bitwise ops for AVM compatibility

pragma solidity ^0.8.20;

// A ShortString is a bytes32 with the string packed left-aligned and length in the last byte.
type ShortString is bytes32;

library ShortStrings {
    /// @dev Packs a string (max 31 bytes) into a ShortString (bytes32).
    /// Layout: string bytes left-aligned in positions 0..len-1, length in byte 31.
    function toShortString(string memory str) internal pure returns (ShortString) {
        bytes memory bstr = bytes(str);
        require(bstr.length <= 31, "StringTooLong");
        // Build uint256 with bytes packed at the high end (left-aligned in bytes32)
        uint256 packed = 0;
        for (uint256 i = 0; i < bstr.length; i++) {
            packed = packed | (uint256(uint8(bstr[i])) << (248 - i * 8));
        }
        // Store length in the last (lowest) byte
        packed = packed | bstr.length;
        return ShortString.wrap(bytes32(packed));
    }

    /// @dev Extracts the string from a ShortString.
    function toString(ShortString sstr) internal pure returns (string memory) {
        uint256 value = uint256(ShortString.unwrap(sstr));
        uint256 len = value & 0xFF;
        bytes memory result = new bytes(len);
        for (uint256 i = 0; i < len; i++) {
            result[i] = bytes1(uint8(value >> (248 - i * 8)));
        }
        return string(result);
    }

    /// @dev Returns the byte length of the string stored in a ShortString.
    function byteLength(ShortString sstr) internal pure returns (uint256) {
        return uint256(ShortString.unwrap(sstr)) & 0xFF;
    }
}

// Test contract
contract ShortStringsTest {
    function toShortString(string memory str) external pure returns (bytes32) {
        return ShortString.unwrap(ShortStrings.toShortString(str));
    }

    function toString(bytes32 sstr) external pure returns (string memory) {
        return ShortStrings.toString(ShortString.wrap(sstr));
    }

    function byteLength(bytes32 sstr) external pure returns (uint256) {
        return ShortStrings.byteLength(ShortString.wrap(sstr));
    }

    function roundTrip(string memory str) external pure returns (string memory) {
        ShortString encoded = ShortStrings.toShortString(str);
        return ShortStrings.toString(encoded);
    }
}
