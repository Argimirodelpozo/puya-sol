contract C {
    // AVM: block.prevrandao maps to `block BlkSeed (global Round - 1)` —
    // the VRF output (block seed) of the previous round. Analogous to EVM
    // prevrandao but not equivalent: both provide pseudo-randomness derived
    // from the previous block's validator output. Values differ.
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
