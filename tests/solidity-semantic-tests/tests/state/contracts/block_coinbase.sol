// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM has no miner/coinbase concept — blocks are produced by rotating
    // validators chosen by VRF. The test driver supplies --evm-coinbase so
    // this EVM environment dependency is explicit rather than fabricated.
    // Original EVM expected: specific miner address from test framework.
    function f() public returns (bool) {
        return block.coinbase != address(0);
    }
}
// ----
// f() -> true
// f() -> true
// f() -> true
