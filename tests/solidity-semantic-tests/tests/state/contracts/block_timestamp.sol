// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM: block.timestamp maps to `global LatestTimestamp` — the Unix
    // timestamp of the latest confirmed block. Analogous to EVM block.timestamp
    // but returns real wall-clock time, not synthetic 15-second increments.
    // Original EVM expected:
    //   constructor() # block 1
    //   f() -> 0x1e  # block 2, 30 seconds
    //   f() -> 0x2d  # block 3, 45 seconds
    constructor() {}
    function f() public returns (bool) {
        return block.timestamp > 0;
    }
}
// ----
// f() -> true
// f() -> true
