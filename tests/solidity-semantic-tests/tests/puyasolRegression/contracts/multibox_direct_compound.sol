// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Direct-element compound assignment on a MULTI-BOX (>32KB) array. The write
// shape used a never-assigned placeholder (__mb_direct) as the write target;
// plain `=` only used its wtype, but the compound read DECODED it — an
// uninitialized var (uint64 0) — instead of reading the element's page:
// signed elements died on `b< wanted bigint but got uint64`, and same-carrier
// shapes would have silently computed against 0.
contract MultiBoxDirectCompound {
    int256[1100] big;      // 35KB
    uint256[1100] ubig;    // 35KB

    function signedAdd() external returns (int256) {
        big[5] = 100;
        int8 d = -5;
        big[5] += d;
        return big[5];     // 95
    }
    function signedNeg() external returns (int256) {
        big[7] = -50;
        int8 d = -8;
        big[7] += d;
        return big[7];     // -58
    }
    // PLAIN assign of a narrow signed value: the shared value pipeline must
    // sign-extend (the multibox copy skipped it: stored 2^64-5 raw).
    function plainNarrow() external returns (int256) {
        int8 d = -5;
        big[9] = d;
        return big[9];   // -5
    }
    function unsignedMul() external returns (uint256) {
        ubig[900] = 7;
        ubig[900] *= 6;
        return ubig[900];  // 42
    }
}
