// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Scratch slots are 4096 bytes, so a 32-byte access at a 32-ALIGNED offset can
// never cross a slot boundary; an unaligned one can and must still stitch two
// slots. These exercise both sides, and the exact 4096 seam.
contract MemSeam {
    // Aligned: provable in-slot, straddle arm should be elided.
    function alignedRoundTrip(uint256 off32, uint256 v) external pure returns (uint256 r) {
        assembly {
            let p := mul(off32, 32)
            mstore(p, v)
            r := mload(p)
        }
    }

    // Unaligned by a constant: must still be correct.
    function unalignedRoundTrip(uint256 base32, uint256 skew, uint256 v)
        external pure returns (uint256 r)
    {
        assembly {
            let p := add(mul(base32, 32), skew)
            mstore(p, v)
            r := mload(p)
        }
    }

    // Straddle the 4096 seam exactly: last aligned word starts at 4064, so
    // 4065..4095 all cross into the next slot.
    function seamRoundTrip(uint256 skew, uint256 v) external pure returns (uint256 r) {
        assembly {
            let p := add(4064, skew)
            mstore(p, v)
            r := mload(p)
        }
    }

    // A neighbouring aligned word must survive an unaligned write next to it.
    function seamNeighbours(uint256 v1, uint256 v2)
        external pure returns (uint256 a, uint256 b)
    {
        assembly {
            mstore(4064, v1)      // aligned, ends exactly at the seam
            mstore(4096, v2)      // aligned, first word of the next slot
            a := mload(4064)
            b := mload(4096)
        }
    }

    // Unaligned write across the seam must not corrupt the words either side.
    function seamCrossingKeepsNeighbours(uint256 mid)
        external pure returns (uint256 before_, uint256 crossed, uint256 after_)
    {
        assembly {
            mstore(4032, 0x1111111111111111111111111111111111111111111111111111111111111111)
            mstore(4096, 0x2222222222222222222222222222222222222222222222222222222222222222)
            mstore(4080, mid)     // 4080..4111 crosses the 4096 boundary
            before_ := mload(4032)
            crossed := mload(4080)
            after_ := mload(4096)
        }
    }
}
