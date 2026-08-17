// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM has an opcode budget rather than an EVM block gas limit. The test
    // driver explicitly supplies the upstream environment's 20,000,000 value
    // with --evm-block-gas-limit.
    function f() public returns (uint) {
        return block.gaslimit;
    }
}
// ----
// f() -> 20000000
// f() -> 20000000
// f() -> 20000000
