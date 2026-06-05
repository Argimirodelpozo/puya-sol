// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Guard for the V4 BalanceDelta pack/unpack with NEGATIVE int128 amounts (the
// remove-liquidity / owed-to-caller deltas). Two int128 packed into an int256:
//   bd = (amount0 << 128) | (amount1 & type(uint128).max)
//   amount0 = sar(128, bd) ; amount1 = signextend(15, bd)
// Negative amounts must round-trip — sar must sign-extend amount0, signextend(15)
// must sign-extend amount1 from bit 127.
type BalanceDelta is int256;

function toBalanceDelta(int128 _a0, int128 _a1) pure returns (BalanceDelta bd) {
    assembly ("memory-safe") {
        bd := or(shl(128, _a0), and(sub(shl(128, 1), 1), _a1))
    }
}

contract BalanceDeltaInt128 {
    function amount0(int128 a0, int128 a1) external pure returns (int128 r) {
        BalanceDelta bd = toBalanceDelta(a0, a1);
        assembly ("memory-safe") { r := sar(128, bd) }
    }

    function amount1(int128 a0, int128 a1) external pure returns (int128 r) {
        BalanceDelta bd = toBalanceDelta(a0, a1);
        assembly ("memory-safe") { r := signextend(15, bd) }
    }
}
