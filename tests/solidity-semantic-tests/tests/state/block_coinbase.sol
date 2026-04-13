contract C {
    // AVM has no miner/coinbase concept — blocks are produced by rotating
    // validators chosen by VRF. block.coinbase is stubbed as the current
    // application's address (non-zero, consistent across calls).
    // Original EVM expected: specific miner address from test framework.
    function f() public returns (bool) {
        return block.coinbase != address(0);
    }
}
// ----
// f() -> true
// f() -> true
// f() -> true
