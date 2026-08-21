// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM puya-sol regression — the "who still assumes
// slot 0" audit). EVM memory is modeled as N scratch slots of 4096 bytes; the
// multi-slot migration reached the range helpers but not the runtime-offset
// word paths, which neither routed past slot 0 nor stitched a word straddling
// a slot boundary.
contract MemorySlotAudit {
    // B7: a RUNTIME offset keeps the word path out of the constant-folding
    // twins. p = 4090 straddles the slot 0/1 boundary (4090 + 32 > 4096);
    // pre-fix the extract3/replace3 ran off the end of the slot and panicked.
    function wordRoundTrip(uint256 p, uint256 v) public pure returns (uint256 r) {
        assembly {
            mstore(p, v)
            r := mload(p)
        }
    }

    // A straddling write must touch exactly its own 32 bytes: the words on
    // either side of it stay as written.
    function straddleNeighbours(uint256 p, uint256 v)
        public pure returns (uint256 lo, uint256 mid, uint256 hi)
    {
        assembly {
            mstore(sub(p, 32), 0x1111)
            mstore(add(p, 32), 0x3333)
            mstore(p, v)
            lo := mload(sub(p, 32))
            mid := mload(p)
            hi := mload(add(p, 32))
        }
    }

    // B7 through the range word-loops: a runtime-length mcopy whose source and
    // destination both straddle the boundary.
    function copyAcrossBoundary(uint256 src, uint256 dst, uint256 len)
        public pure returns (uint256 a, uint256 b)
    {
        assembly {
            mstore(src, 0xaaaa)
            mstore(add(src, 32), 0xbbbb)
            mcopy(dst, src, len)
            a := mload(dst)
            b := mload(add(dst, 32))
        }
    }

    // B8: `keccak256(off, 0x20)` at a constant offset folds at COMPILE TIME
    // from the tracked mem_0x<off> word. mstore8 (statement position) never
    // invalidated that tracking, so the second hash was folded over the STALE
    // pre-mstore8 word — the same silent-wrong-slot hazard for
    // `sstore(keccak256(...), v)`.
    function keccakAfterMstore8() public pure returns (bytes32 h1, bytes32 h2) {
        assembly {
            mstore(0x80, 1)
            h1 := keccak256(0x80, 0x20)
            mstore8(0x80, 0xff)
            h2 := keccak256(0x80, 0x20)
        }
    }

    // Same invalidation gap through a statement-position mcopy.
    function keccakAfterMcopy() public pure returns (bytes32 h1, bytes32 h2) {
        assembly {
            mstore(0x80, 1)
            mstore(0xc0, 2)
            h1 := keccak256(0x80, 0x20)
            mcopy(0x80, 0xc0, 32)
            h2 := keccak256(0x80, 0x20)
        }
    }
}
