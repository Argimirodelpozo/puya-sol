contract C {
    // AVM: no EIP-1559 base fee concept — transactions pay a flat per-txn
    // fee (typically 1000 microAlgos). block.basefee is stubbed as 0.
    // Original EVM expected: f() -> 7, g() -> 7
    function f() public view returns (uint) {
        return block.basefee;
    }
    function g() public view returns (uint ret) {
        assembly {
            ret := basefee()
        }
    }
}
// ====
// EVMVersion: >=london
// ----
// f() -> 0
// g() -> 0
// f() -> 0
// g() -> 0
