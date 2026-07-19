// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the assembly constant-cache invalidation: m_localConstants entries were
// recorded at `let` declarations but never invalidated on `:=` assignment (and
// mem-content "mem_0x<off>" entries never invalidated at all), so the canonical
// pointer-bump and indexed-loop idioms folded every store to the STALE constant
// offset, and keccak/mload folds read stale constant contents.
contract AsmConstCache {
    // Pointer bump: second mstore must land at 0xa0, not fold back to 0x80.
    function reassign() external pure returns (uint256 r) {
        assembly {
            let p := 0x80
            mstore(p, 111)
            p := add(p, 0x20)
            mstore(p, 222)
            r := mload(0x80)
        }
    }

    // Loop-indexed store: offsets must stay runtime; mem[0xa0] sees i=1's write.
    function loopFold() external pure returns (uint256 r) {
        assembly {
            for { let i := 0 } lt(i, 3) { i := add(i, 1) } {
                mstore(add(0x80, mul(i, 0x20)), add(i, 1))
            }
            r := mload(0xa0)
        }
    }

    // Content staleness: the hash must depend on x, not fold to keccak(5).
    function kec(uint256 x) external pure returns (bytes32 h) {
        assembly {
            mstore(0x80, 5)
            mstore(0x80, x)
            h := keccak256(0x80, 0x20)
        }
    }

    // A branch-recorded content constant must not fold after the if.
    function branch(uint256 c) external pure returns (uint256 r) {
        assembly {
            mstore(0x80, 1)
            if c { mstore(0x80, 7) }
            r := mload(0x80)
        }
    }
}
