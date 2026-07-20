// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards four asm/storage semantics fixes:
// (1) transient sub-64 signed reads sign-extend (int32 x = -1 read +4294967295);
// (2) keccak256(constOff, len) hashes the EXACT length (0x30 hashed only 32 B);
// (3) inlined Yul fn BODY locals alpha-rename (nested helpers sharing `t`
//     clobbered one runtime var: 110 instead of 106);
// (4) Yul call args evaluate RIGHT-to-left (sub(bump(1), bump(10)) ran the
//     left bump first).
contract AsmSemanticsBatch {
    int32 transient tx32;

    function transientSigned() external returns (int32 r, bool neg) {
        tx32 = -1;
        r = tx32;
        neg = tx32 < 0;
    }

    function kec48(uint256 a, uint256 b) external pure returns (bytes32 h) {
        assembly {
            mstore(0x80, a)
            mstore(0xa0, b)
            h := keccak256(0x80, 0x30) // 48 bytes: word a + first 16 bytes of b
        }
    }

    function inlineLocals(uint256 b) external pure returns (uint256 z) {
        assembly {
            function g(x) -> r {
                let t := 5
                r := add(x, t)
            }
            function f(y) -> r {
                let t := 1
                r := add(g(y), t) // g's t=5 must not clobber f's t=1
            }
            z := add(f(b), 0)
        }
    }

    function argOrder() external pure returns (uint256 diff, uint256 count) {
        assembly {
            function bump(v) -> r {
                let c := mload(0x80)
                mstore(0x80, add(c, 1))
                r := add(v, c)
            }
            // Yul: right-to-left → bump(10) sees c=0 (→10), bump(1) sees c=1 (→2)
            let s := sub(bump(1), bump(10)) // 2 - 10 = -8 mod 2^256
            diff := sub(0, s)               // 8 (pre-fix L-to-R gave 10)
            count := mload(0x80)
        }
    }
}
