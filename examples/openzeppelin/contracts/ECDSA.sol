// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/// @dev Utilities for creating Ethereum-compatible message hashes.
library MessageHashUtils {
    /// @dev Returns the keccak256 digest of an EIP-191 signed data with version 0x45 (personal message).
    /// The digest is: keccak256("\x19Ethereum Signed Message:\n" || len || message)
    /// For a fixed 32-byte hash, len is always "32".
    function toEthSignedMessageHash(bytes32 hash) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked("\x19Ethereum Signed Message:\n32", hash));
    }

    /// @dev Returns the keccak256 digest of an EIP-712 typed data.
    /// The digest is: keccak256("\x19\x01" || domainSeparator || structHash)
    function toTypedDataHash(bytes32 domainSeparator, bytes32 structHash) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked("\x19\x01", domainSeparator, structHash));
    }
}

/// @dev Simplified ECDSA library — hash utilities only (ecrecover has AVM differences).
library ECDSA {
    enum RecoverError {
        NoError,
        InvalidSignature,
        InvalidSignatureLength,
        InvalidSignatureS
    }

    error ECDSAInvalidSignature();
    error ECDSAInvalidSignatureLength(uint256 length);
    error ECDSAInvalidSignatureS(bytes32 s);

    /// @dev Half of the secp256k1 curve order (for signature malleability check).
    bytes32 internal constant _S_UPPER_BOUND = 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF5D576E7357A4501DDFE92F46681B20A0;
}

contract ECDSATest {
    function testToEthSignedMessageHash(bytes32 hash) external pure returns (bytes32) {
        return MessageHashUtils.toEthSignedMessageHash(hash);
    }

    function testToTypedDataHash(bytes32 domainSeparator, bytes32 structHash) external pure returns (bytes32) {
        return MessageHashUtils.toTypedDataHash(domainSeparator, structHash);
    }

    function testKeccak256Packed(bytes32 a, bytes32 b) external pure returns (bytes32) {
        return keccak256(abi.encodePacked(a, b));
    }
}
