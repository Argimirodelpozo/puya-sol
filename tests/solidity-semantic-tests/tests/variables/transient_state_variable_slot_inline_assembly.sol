// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// ----------------------------------------------------------------------------
// Modification: `h()` expected return changed from `1, 1` to `2, 0`.
//
// Reason: `address` on AVM is a 32-byte account public key, not EVM's 20-byte
// address. Our transient storage layout stores `address transient a` at its
// native 32-byte width (see src/builder/storage/TransientStorage.cpp).
// A 32-byte variable cannot coexist in the same slot as `int8 transient w`
// (1 byte, already at slot 1 offset 0), so `a` moves to slot 2 offset 0
// rather than slot 1 offset 1 under EVM's 20-byte-address packing.
//
// The 20-byte layout destroyed the top 12 bytes of any assigned Algorand
// address, breaking `a.balance` / `acct_params_get` round-tripping (see
// tests/variables/transient_state_address_variable_members.sol). The
// architectural tradeoff is documented; this test was adjusted to match it.
//
// Original EVM expectations (for reference):
//   // f() -> 0, 0
//   // g() -> 1, 0
//   // h() -> 1, 1
// ============================================================================
contract C {
    uint256 y;
    uint256 transient x;
    int8 transient w;
    int z;
    address transient a;
    function f() public returns(uint256 s, uint256 o) {
        assembly {
            s := x.slot
            o := x.offset
        }
    }
    function g() public returns(uint256 s, uint256 o) {
        assembly {
            s := w.slot
            o := w.offset
        }
    }
    function h() public returns(uint256 s, uint256 o) {
        assembly {
            s := a.slot
            o := a.offset
        }
    }
}
// ====
// EVMVersion: >=cancun
// ----
// f() -> 0, 0
// g() -> 1, 0
// h() -> 2, 0
