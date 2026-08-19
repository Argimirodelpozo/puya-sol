// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM puya-sol regression — new_review.md A2: with receive()/fallback()
// present the hand-written selector dispatch approved bare and
// unmatched-selector calls WITHOUT reading Txn.OnCompletion, so anyone could
// Delete/Update/CloseOut the app. Both dispatch arms are NoOp-only now.
contract FallbackOC {
    uint256 public hits;

    receive() external payable { hits += 1; }

    fallback() external payable { hits += 100; }
}
