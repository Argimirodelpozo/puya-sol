// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See inline comments for AVM adaptation notes.
// ============================================================================
contract C {
    // AVM: no blob data pricing on Algorand — block.blobbasefee is stubbed
    // as 0. Original EVM expected: f() -> 1, g() -> 1
    function f() public view returns (uint) {
        return block.blobbasefee;
    }
    function g() public view returns (uint ret) {
        assembly {
            ret := blobbasefee()
        }
    }
}
// ====
// EVMVersion: >=cancun
// ----
// f() -> 0
// g() -> 0
// f() -> 0
// g() -> 0
