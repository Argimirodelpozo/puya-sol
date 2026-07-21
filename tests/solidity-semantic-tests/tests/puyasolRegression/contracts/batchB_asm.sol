// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards Batch B asm/Yul fixes (fable-review-3):
// M6  a Yul `if` body with a conditional `leave` BEFORE a revert must not
//     collapse to assert(!cond) — the leave-path must still be reachable;
// M11 transient tload/tstore of a slot >= 128 (or a mapping) reverts loudly;
// M12 dynamic-offset calldataload past calldatasize zero-pads (no panic).
contract BatchBAsm {
    // M6: leave (early return) before revert; when x != 0 the function returns
    // r without reverting.
    function m6(uint256 c, uint256 x) external pure returns (uint256 r) {
        assembly {
            function check(cc, xx) -> rr {
                rr := 7
                if cc {
                    if xx { rr := 42 leave }
                    revert(0, 0)
                }
            }
            r := check(c, x)
        }
    }

    // M12: read a word well past the (tiny) calldata → all zero, no panic.
    function m12(uint256 farOffset) external pure returns (uint256 w) {
        assembly {
            w := calldataload(farOffset)
        }
    }

    // M11: tstore/tload of a slot >= 128 must revert (out of the 128-slot blob).
    function m11Bad(uint256 slot) external returns (uint256 v) {
        assembly {
            tstore(slot, 9)
            v := tload(slot)
        }
    }

    // M11 ok: small transient slot round-trips.
    function m11Ok() external returns (uint256 v) {
        assembly {
            tstore(3, 99)
            v := tload(3)
        }
    }
}
