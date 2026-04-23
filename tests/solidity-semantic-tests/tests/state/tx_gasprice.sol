// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM: tx.gasprice maps to `txn Fee` — the flat transaction fee in
    // microAlgos. NOT equivalent to EVM gas price (wei per gas unit).
    // AVM uses a flat fee model (typically 1000 microAlgos per txn),
    // not a per-opcode gas price. The value is always > 0.
    // Original EVM expected:
    //   f() -> 3000000000 (3 gwei, x3)
    function f() public returns (bool) {
        return tx.gasprice > 0;
    }
}
// ----
// f() -> true
// f() -> true
// f() -> true
