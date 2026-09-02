// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Solidity's allocator only hands out 32-aligned pointers, so an offset of
// "free memory pointer + aligned constant" is aligned too — the shape every
// snarkjs/Groth16 verifier uses. An unaligned bump anywhere in the block
// must poison that for the whole block.
contract FmpAlignment {
    function alignedBump(uint256 v) external pure returns (uint256 r) {
        assembly {
            let pMem := mload(0x40)
            mstore(0x40, add(pMem, 0x200))
            mstore(add(pMem, 0x40), v)
            r := mload(add(pMem, 0x40))
        }
    }

    function unalignedBump(uint256 v) external pure returns (uint256 r) {
        assembly {
            let pMem := mload(0x40)
            mstore(0x40, add(pMem, 0x21))
            mstore(add(pMem, 0x40), v)
            r := mload(add(pMem, 0x40))
        }
    }

    // Reads and writes either side of an unaligned bump must still land right.
    function unalignedBumpNeighbours(uint256 a, uint256 b)
        external pure returns (uint256 x, uint256 y)
    {
        assembly {
            let pMem := mload(0x40)
            mstore(0x40, add(pMem, 0x41))
            mstore(add(pMem, 0x20), a)
            mstore(add(pMem, 0x40), b)
            x := mload(add(pMem, 0x20))
            y := mload(add(pMem, 0x40))
        }
    }
}
