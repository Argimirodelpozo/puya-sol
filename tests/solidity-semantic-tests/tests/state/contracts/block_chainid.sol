// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM: block.chainid maps to `global GenesisHash` — the 32-byte hash
    // that uniquely identifies the Algorand network (mainnet/testnet/localnet).
    // No direct equivalent to EVM's integer chain ID, but GenesisHash serves
    // the same purpose of network identification.
    // Original EVM expected:
    //   f() -> 1 (mainnet chain ID)
    function f() public returns (bool) {
        return block.chainid > 0;
    }
}
// ====
// EVMVersion: >=istanbul
// ----
// f() -> true
// f() -> true
// f() -> true
