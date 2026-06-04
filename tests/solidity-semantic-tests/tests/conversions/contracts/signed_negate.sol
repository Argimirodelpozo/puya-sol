// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Guard for unary-minus + uint256() cast on a NEGATIVE int256 — the V4
// SwapMath.computeSwapStep exact-in path `uint256(-amountRemaining)`. If the
// negation of a 256-bit two's-complement negative miscompiles (to 0 or a huge
// value), amountRemainingLessFee goes wrong and the swap step makes no progress.
contract SignedNegate {
    function negToUint(int256 x) external pure returns (uint256) {
        return uint256(-x);
    }

    function isNeg(int256 x) external pure returns (bool) {
        return x < 0;
    }

    // computeSwapStep's amountRemainingLessFee shape: uint256(-amount)*(1e6-fee)/1e6
    function lessFee(int256 amountRemaining, uint24 feePips) external pure returns (uint256) {
        return (uint256(-amountRemaining) * (1000000 - feePips)) / 1000000;
    }
}
