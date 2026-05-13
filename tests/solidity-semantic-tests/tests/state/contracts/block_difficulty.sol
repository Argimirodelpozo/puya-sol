// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM: block.difficulty returns 0 — Algorand has no proof-of-work.
    // Pre-Paris EVM returned the mining difficulty; post-Paris it was
    // replaced by prevrandao. On AVM, use block.prevrandao for randomness.
    // Original EVM expected:
    //   f() -> 200000000 (x3)
    function f() public returns (uint) {
        return block.difficulty;
    }
}
// ====
// EVMVersion: <paris
// ----
// f() -> 0
// f() -> 0
// f() -> 0
