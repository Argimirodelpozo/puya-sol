// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM: block.prevrandao maps to block seed of round-1 (see block_prevrandao.sol).
    // Pre-Paris EVM returned block difficulty; on AVM both map to the same block seed.
    // Original EVM expected: f() -> 200000000
    function f() public view returns (bool) {
        return block.prevrandao > 0;
    }
}
// ====
// EVMVersion: <paris
// ----
// f() -> true
