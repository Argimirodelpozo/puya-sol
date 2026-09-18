// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

contract YulWordReductions {
    function largeSignextend(uint256 x) external pure returns (uint256 a, uint256 b) {
        assembly {
            a := signextend(0x10000000000000000, x)
            b := signextend(0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff, x)
        }
    }

    function words(uint256 x, uint256 n) external pure returns (uint256 a, uint256 b, uint256 c, uint256 d) {
        assembly { a := shl(n, x) b := shr(n, x) c := sar(n, x) d := signextend(n, x) }
    }

    function typed(uint256 x, uint256 n) external pure returns (uint256, uint256, uint256) {
        return (x << n, x >> n, uint256(int256(x) >> n));
    }

    function ordered(uint256 x, uint256 n, uint256 op) external pure returns (uint256 r, uint256 trace) {
        assembly {
            function mark(v, tag) -> result {
                mstore(0, add(mul(mload(0), 10), tag))
                result := v
            }
            mstore(0, 0)
            switch op
            case 0 { r := shl(mark(n, 1), mark(x, 2)) }
            case 1 { r := shr(mark(n, 1), mark(x, 2)) }
            case 2 { r := sar(mark(n, 1), mark(x, 2)) }
            default { r := signextend(mark(n, 1), mark(x, 2)) }
            trace := mload(0)
        }
    }

    function literals() external pure returns (bytes32 a, bytes32 b, bytes32 c, bytes32 d) {
        assembly {
            a := "abc"
            b := hex"0001ff00"
            c := "\x00\"\\\xff"
            d := ""
        }
    }
}
