contract C {
    // AVM has a fixed opcode budget per app call (700), poolable across a
    // 16-txn group. block.gaslimit is stubbed as a sentinel 70000 so
    // Solidity patterns that gate on `gaslimit > N` still make progress.
    // Original EVM expected: f() -> 20000000
    function f() public returns (uint) {
        return block.gaslimit;
    }
}
// ----
// f() -> 70000
// f() -> 70000
// f() -> 70000
