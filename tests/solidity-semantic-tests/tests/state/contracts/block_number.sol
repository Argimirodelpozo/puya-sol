// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM: block.number maps to `global Round` — the current Algorand round.
    // Analogous to EVM block number but values differ (localnet starts at a
    // high round, increments per transaction). We test that it returns > 0.
    // Original EVM expected:
    //   constructor()
    //   f() -> 2
    //   f() -> 3
    constructor() {}
    function f() public returns (bool) {
        return block.number > 0;
    }
}
// ----
// f() -> true
// f() -> true
