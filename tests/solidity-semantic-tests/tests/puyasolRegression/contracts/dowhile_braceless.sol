// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM puya-sol regression — new_review.md A1: the do-while arm translated
// the body only when it was a Block, so a brace-less body was silently
// DROPPED. With a side-effecting condition puya's infinite-loop detector
// stays quiet and the function compiles clean with an empty loop body.
contract DoWhileBraceless {
    uint256 public n;

    function bump() internal returns (bool) {
        n += 1;
        return n < 3;
    }

    // Pre-fix: body gone, returned 0 (EVM: 30) with zero diagnostics.
    function bracelessSideEffectCond() public returns (uint256 acc) {
        do acc += 10; while (bump());
    }

    // Pre-fix: puya "infinite loop detected" (body dropped, pure condition).
    function bracelessSimple() public pure returns (uint256) {
        uint256 i = 0;
        do i++; while (i < 3);
        return i;
    }

    // Brace-less terminating statement as the body.
    function bracelessReturn(uint256 x) public pure returns (uint256) {
        do return x + 1; while (x < 5);
    }
}
