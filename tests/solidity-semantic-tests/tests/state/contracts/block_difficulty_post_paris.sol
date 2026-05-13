// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM: block.difficulty returns 0 — Algorand has no proof-of-work.
    // Post-Paris EVM returns prevrandao here; on AVM use block.prevrandao.
    // Original EVM expected:
    //   f() -> 0xa86c2e601b6c44eb... (x3, same as prevrandao)
    function f() public returns (uint) {
        return block.difficulty;
    }
}
// ====
// EVMVersion: >=paris
// ----
// f() -> 0
// f() -> 0
// f() -> 0
