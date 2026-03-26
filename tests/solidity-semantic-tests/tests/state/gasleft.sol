contract C {
    function f() public returns (bool) {
        return gasleft() > 0;
    }
}
// ====
// bytecodeFormat: legacy
// ----
// AVM: gasleft() maps to `global OpcodeBudget` which returns the remaining
// opcode budget for the transaction group. Analogous but not equivalent to
// EVM gas — units and scale differ (budget = group_size * 700, decreases
// per opcode). The test checks gasleft() > 0, which holds on both platforms.
// Original EVM expected values (unchanged — both return true):
// f() -> true
// f() -> true
// f() -> true
