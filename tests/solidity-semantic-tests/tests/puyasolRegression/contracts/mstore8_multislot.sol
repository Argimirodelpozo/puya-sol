// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards mstore8 multi-slot routing (fable-review-3 M7): mstore8 used a
// slot-0-only replace3, so any offset >= 4096 (routine once the free-memory
// pointer passes 4KB) panicked or mis-wrote. Now it uses the same runtime
// slot math (off/SLOT_SIZE, off%SLOT_SIZE) as the slot-aware mstore.
contract Mstore8Multislot {
    function writeHigh(uint256 off, uint8 b) external pure returns (uint256 r) {
        assembly {
            mstore8(off, b)
            r := shr(248, mload(off)) // top byte of the word at off == memory[off]
        }
    }
}
