// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

library NewReviewSelectorLib {
    function matches() internal view returns (bool) {
        return msg.sig == NewReviewSelectorContext.probe.selector;
    }
}

function newReviewFreeMatches() view returns (bool) {
    return msg.sig == NewReviewSelectorContext.probe.selector;
}

// D6: library/free functions translated without currentContract still know
// every deployable contract's ARC-4-to-EVM selector routes.
contract NewReviewSelectorContext {
    function probe() external view returns (bool, bool) {
        return (NewReviewSelectorLib.matches(), newReviewFreeMatches());
    }
}
