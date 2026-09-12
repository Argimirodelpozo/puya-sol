// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // Opted-in AVM adaptation: seed at transaction FirstValid - 1 (zero
    // at FirstValid 0). Same as adapted block.prevrandao, not secure randomness.
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
