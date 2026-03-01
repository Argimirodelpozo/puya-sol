// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0) (utils/Strings.sol)
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/utils/Strings.sol
// Modification: Assembly in toString replaced with pure Solidity, Math.log10/log256 inlined

pragma solidity ^0.8.20;

/**
 * @dev String operations.
 */
library Strings {
    error StringsInsufficientHexLength(uint256 value, uint256 length);

    function _hexDigit(uint256 d) private pure returns (bytes1) {
        if (d < 10) return bytes1(uint8(48 + d));  // '0'-'9'
        return bytes1(uint8(87 + d));               // 'a'-'f'
    }

    /**
     * @dev Converts a `uint256` to its ASCII `string` decimal representation.
     */
    function toString(uint256 value) internal pure returns (string memory) {
        if (value == 0) return "0";
        uint256 temp = value;
        uint256 digits;
        while (temp != 0) {
            digits++;
            temp /= 10;
        }
        bytes memory buffer = new bytes(digits);
        while (value != 0) {
            digits -= 1;
            buffer[digits] = bytes1(uint8(48 + uint256(value % 10)));
            value /= 10;
        }
        return string(buffer);
    }

    /**
     * @dev Converts a `int256` to its ASCII `string` decimal representation.
     */
    function toStringSigned(int256 value) internal pure returns (string memory) {
        return string.concat(value < 0 ? "-" : "", toString(uint256(value < 0 ? -value : value)));
    }

    /**
     * @dev Converts a `uint256` to its ASCII `string` hexadecimal representation.
     */
    function toHexString(uint256 value) internal pure returns (string memory) {
        if (value == 0) return "0x00";
        uint256 temp = value;
        uint256 length;
        while (temp != 0) {
            length++;
            temp >>= 8;
        }
        return toHexString(value, length);
    }

    /**
     * @dev Converts a `uint256` to its ASCII `string` hexadecimal representation with fixed length.
     */
    function toHexString(uint256 value, uint256 length) internal pure returns (string memory) {
        uint256 localValue = value;
        bytes memory buffer = new bytes(2 * length + 2);
        buffer[0] = "0";
        buffer[1] = "x";
        for (uint256 i = 2 * length + 1; i > 1; --i) {
            buffer[i] = _hexDigit(localValue & 0xf);
            localValue >>= 4;
        }
        if (localValue != 0) {
            revert StringsInsufficientHexLength(value, length);
        }
        return string(buffer);
    }

    /**
     * @dev Converts an `address` to its checksumless ASCII `string` hexadecimal representation.
     */
    function toHexString(address addr) internal pure returns (string memory) {
        return toHexString(uint256(uint160(addr)), 20);
    }

    /**
     * @dev Returns true if the two strings are equal.
     */
    function equal(string memory a, string memory b) internal pure returns (bool) {
        return bytes(a).length == bytes(b).length && keccak256(bytes(a)) == keccak256(bytes(b));
    }
}

// Test contract
contract StringsExpandedTest {
    using Strings for uint256;
    using Strings for int256;
    using Strings for address;

    function toString(uint256 value) external pure returns (string memory) {
        return value.toString();
    }

    function toStringSigned(int256 value) external pure returns (string memory) {
        return value.toStringSigned();
    }

    function toHexString(uint256 value) external pure returns (string memory) {
        return value.toHexString();
    }

    function toHexStringFixed(uint256 value, uint256 length) external pure returns (string memory) {
        return value.toHexString(length);
    }

    function toHexStringAddr(address addr) external pure returns (string memory) {
        return addr.toHexString();
    }

    function equal(string memory a, string memory b) external pure returns (bool) {
        return Strings.equal(a, b);
    }
}
