// SPDX-License-Identifier: MIT
// Based on OpenZeppelin Contracts v5.0.0 (utils/Create2.sol)
// Adapted: only the address computation function, not actual deployment

pragma solidity ^0.8.20;

/**
 * @dev Demonstrates CREATE2 address computation (deterministic deployment).
 * On Algorand this is just the hash computation, not actual deployment.
 */
contract Create2Test {
    /**
     * @dev Returns the address where a contract will be stored if deployed via
     * CREATE2 from a contract at `deployer` with `salt` and `bytecodeHash`.
     */
    function computeAddress(
        bytes32 salt,
        bytes32 bytecodeHash,
        address deployer
    ) external pure returns (address) {
        return address(uint160(uint256(keccak256(
            abi.encodePacked(bytes1(0xff), deployer, salt, bytecodeHash)
        ))));
    }

    /**
     * @dev Returns the keccak256 hash of creation bytecode.
     */
    function computeBytecodeHash(bytes memory bytecode) external pure returns (bytes32) {
        return keccak256(bytecode);
    }

    /**
     * @dev Verify that the same inputs always produce the same address.
     */
    function verifyDeterministic(
        bytes32 salt1,
        bytes32 salt2,
        bytes32 bytecodeHash,
        address deployer
    ) external pure returns (bool) {
        address addr1 = address(uint160(uint256(keccak256(
            abi.encodePacked(bytes1(0xff), deployer, salt1, bytecodeHash)
        ))));
        address addr2 = address(uint160(uint256(keccak256(
            abi.encodePacked(bytes1(0xff), deployer, salt2, bytecodeHash)
        ))));
        // Same salt → same address, different salt → different address
        return (salt1 == salt2) == (addr1 == addr2);
    }
}
