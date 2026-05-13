// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// ============================================================
// f() unchanged — `this.f.selector` is compile-time folded to
// keccak256, matching upstream EVM expectations.
//
// h(function) input changed: external fn-ptr is 12 bytes on AVM
// (8-byte appId + 4-byte ARC4 selector), not 24 bytes (20-byte
// address + 4-byte selector) as on EVM. Original input was a
// 24-byte EVM-style value; replaced with a 12-byte AVM-style value
// whose selector slot (bytes 8..11) is `0x42424242`. `a.selector`
// reads bytes 8..11 either way, so the expected output `0x42424242`
// is unchanged.
//
// Originally upstream:
//   h(function): left(0x1122334400112233445566778899AABBCCDDEEFF42424242) -> left(0x42424242)
// AVM-native: 12-byte input
//   h(function): left(0x000000000000000042424242) -> left(0x42424242)
// ============================================================

contract C {
    function f() external returns (bytes4) {
        return this.f.selector;
    }
    function h(function() external a) public returns (bytes4) {
        return a.selector;
    }
}
// ----
// f() -> left(0x26121ff0)
// h(function): left(0x000000000000000042424242) -> left(0x42424242)
