// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // Opted-in AVM adaptation: seed at transaction FirstValid - 1 (zero
    // at FirstValid 0). The caller selects the validity window and the seed
    // is already known: this is not secure randomness or EVM prevrandao.
    // Original EVM expected:
    //   f() -> 0xa86c2e601b6c44eb4848f7d23d9df3113fbcac42041c49cbed5000cb4f118777
    function f() public view returns (bool) {
        return block.prevrandao > 0;
    }
}
// ====
// EVMVersion: >=paris
// ----
// f() -> true
