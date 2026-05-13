// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM: tx.origin maps to `txn Sender` — same as msg.sender on AVM
    // since there's no contract-to-contract call chain distinction.
    // Original EVM expected:
    //   f() -> 0x9292929292929292929292929292929292929292 (x3)
    function f() public returns (bool) {
        return tx.origin != address(0);
    }
}
// ----
// f() -> true
// f() -> true
// f() -> true
