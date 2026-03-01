// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0)
// Flattened: MessageHashUtils library + Strings stub
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/utils/cryptography/MessageHashUtils.sol
// Modification: Assembly blocks replaced with pure Solidity equivalents (abi.encodePacked + keccak256)

pragma solidity ^0.8.20;

// --- Strings stub (toString for uint256) ---
library Strings {
    bytes16 private constant HEX_DIGITS = "0123456789abcdef";

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
}

// --- MessageHashUtils.sol (utils/cryptography/MessageHashUtils.sol) ---

/**
 * @dev Signature message hash utilities for producing digests to be consumed by ECDSA recovery.
 * Provides methods conforming to EIP-191 and EIP-712 specifications.
 */
library MessageHashUtils {
    /**
     * @dev Returns the keccak256 digest of an EIP-191 signed data with version
     * `0x45` (`personal_sign` messages).
     * Equivalent to: keccak256("\x19Ethereum Signed Message:\n32" || messageHash)
     */
    function toEthSignedMessageHash(bytes32 messageHash) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked("\x19Ethereum Signed Message:\n32", messageHash));
    }

    /**
     * @dev Returns the keccak256 digest of an EIP-191 signed data with version
     * `0x45` (`personal_sign` messages) for arbitrary-length messages.
     */
    function toEthSignedMessageHash(bytes memory message) internal pure returns (bytes32) {
        return keccak256(
            abi.encodePacked(
                "\x19Ethereum Signed Message:\n",
                bytes(Strings.toString(message.length)),
                message
            )
        );
    }

    /**
     * @dev Returns the keccak256 digest of an EIP-191 signed data with version
     * `0x00` (data with intended validator).
     */
    function toDataWithIntendedValidatorHash(address validator, bytes memory data) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked(hex"19_00", validator, data));
    }

    /**
     * @dev Returns the keccak256 digest of an EIP-712 typed data (EIP-191 version `0x01`).
     */
    function toTypedDataHash(bytes32 domainSeparator, bytes32 structHash) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked(hex"19_01", domainSeparator, structHash));
    }
}

// Test contract
contract MessageHashUtilsTest {
    using MessageHashUtils for bytes32;
    using MessageHashUtils for bytes;

    function ethSignedMessageHash(bytes32 messageHash) external pure returns (bytes32) {
        return messageHash.toEthSignedMessageHash();
    }

    function ethSignedMessageHashBytes(bytes memory message) external pure returns (bytes32) {
        return message.toEthSignedMessageHash();
    }

    function dataWithIntendedValidatorHash(address validator, bytes memory data) external pure returns (bytes32) {
        return MessageHashUtils.toDataWithIntendedValidatorHash(validator, data);
    }

    function typedDataHash(bytes32 domainSeparator, bytes32 structHash) external pure returns (bytes32) {
        return MessageHashUtils.toTypedDataHash(domainSeparator, structHash);
    }
}
