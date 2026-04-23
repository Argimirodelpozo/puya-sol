// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// ----------------------------------------------------------------------------
// AVM adaptation: blockhash() maps to AVM 'block BlkSeed' (VRF output).
// AVM has no block hash field — BlkSeed is the closest unique-per-block value.
// The test checks that blockhash returns a non-zero value for valid rounds,
// rather than matching specific EVM hash values.
//
// Original EVM expected values:
// f() -> 0x3737373737373737373737373737373737373737373737373737373737373738
// g() -> 0x3737373737373737373737373737373737373737373737373737373737373739
// h() -> 0x373737373737373737373737373737373737373737373737373737373737373a
contract C {
    function test() public returns(bool) {
        // blockhash of round 1 should be non-zero on any live network
        bytes32 h = blockhash(1);
        return h != bytes32(0);
    }
}
// ----
// test() -> true
