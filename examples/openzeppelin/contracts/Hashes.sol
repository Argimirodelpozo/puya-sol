// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.1.0) (utils/cryptography/Hashes.sol)
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/master/contracts/utils/cryptography/Hashes.sol
// MODIFIED — inline assembly replaced with pure Solidity equivalents

pragma solidity ^0.8.20;

/**
 * @dev Library of standard hash functions.
 *
 * _Available since v5.1._
 */
library Hashes {
    /**
     * @dev Commutative Keccak256 hash of a sorted pair of bytes32. Frequently used when working with merkle proofs.
     *
     * NOTE: Equivalent to the `standardNodeHash` in our
     * https://github.com/OpenZeppelin/merkle-tree[JavaScript library].
     */
    function commutativeKeccak256(bytes32 a, bytes32 b) internal pure returns (bytes32) {
        return a < b ? efficientKeccak256(a, b) : efficientKeccak256(b, a);
    }

    /**
     * @dev Implementation of keccak256(abi.encode(a, b)) that doesn't allocate or expand memory.
     *
     * NOTE: Original uses inline assembly; replaced with pure Solidity equivalent for AVM compilation.
     */
    function efficientKeccak256(bytes32 a, bytes32 b) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked(a, b));
    }
}

// Test contract — standard usage of OZ Hashes (not part of OZ source)
contract HashesTest {
    function commutativeKeccak256(bytes32 a, bytes32 b) external pure returns (bytes32) {
        return Hashes.commutativeKeccak256(a, b);
    }

    function efficientKeccak256(bytes32 a, bytes32 b) external pure returns (bytes32) {
        return Hashes.efficientKeccak256(a, b);
    }
}
