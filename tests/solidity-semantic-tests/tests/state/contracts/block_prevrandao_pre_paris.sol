// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // Opted-in AVM seed at FirstValid - 1, not secure randomness (see block_prevrandao.sol).
    // Pre-Paris EVM returned mining difficulty; AVM uses the same adapted seed for both names.
    // Original EVM expected: f() -> 200000000
    function f() public view returns (bool) {
        return block.prevrandao > 0;
    }
}
// ====
// EVMVersion: <paris
// ----
// f() -> true
