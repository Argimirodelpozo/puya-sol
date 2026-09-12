// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // Opted-in AVM adaptation: seed at transaction FirstValid - 1 (zero
    // at FirstValid 0), not mining difficulty. Known in advance and
    // caller-selectable; neither this nor block.prevrandao is secure randomness.
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
