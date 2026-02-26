// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

contract ModExpTest {
    /// Modular exponentiation: base^exponent % modulus
    /// Uses the EVM modexp precompile at address 0x05.
    function modexp(
        uint256 base, uint256 exponent, uint256 modulus
    ) external view returns (uint256 result) {
        assembly {
            // Store lengths (each operand is 32 bytes)
            mstore(0x00, 32)   // Bsize
            mstore(0x20, 32)   // Esize
            mstore(0x40, 32)   // Msize
            // Store operands
            mstore(0x60, base)
            mstore(0x80, exponent)
            mstore(0xa0, modulus)
            // Call modexp precompile: input at 0x00 (192 bytes), output at 0xc0 (32 bytes)
            let success := staticcall(gas(), 5, 0x00, 0xc0, 0xc0, 0x20)
            result := mload(0xc0)
        }
    }
}
