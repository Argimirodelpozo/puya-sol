contract C {
    // AVM: msg.sender maps to `txn Sender` — the 32-byte Algorand address
    // of the transaction sender. Different format from EVM's 20-byte address.
    // Original EVM expected:
    //   f() -> 0x1212121212121212121212121212120000000012
    function f() public returns (bool) {
        return msg.sender != address(0);
    }
}
// ----
// f() -> true
