// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// ============================================================
// f() and g() unchanged — `this.f.address` and `C(addr).f.address`
// are compile-time folded to `address(this)` / the literal addr,
// matching upstream EVM expectations.
//
// h(function) input changed: external fn-ptr is 12 bytes on AVM
// (8-byte appId + 4-byte ARC4 selector), not 24 bytes (20-byte
// address + 4-byte selector) as on EVM. Original input was a
// 24-byte EVM-style value; replaced with a 12-byte AVM-style value
// whose appId portion (bytes 0..7) decodes to 0x1234. `a.address`
// returns the 8-byte appId padded to a 32-byte address.
//
// Originally upstream:
//   h(function): left(0x1122334400112233445566778899AABBCCDDEEFF42424242) -> 0x1122334400112233445566778899AABBCCDDEEFF
// AVM-native: 12-byte input
//   h(function): left(0x000000000000123442424242) -> 0x1234
// ============================================================

contract C {
    function f() external returns (address) {
        return C(address(0x1234)).f.address;
    }
    function g() external returns (bool, bool) {
        return (
            this.f.address == address(this),
            C(address(0x1234)).f.address == address(0x1234)
        );
    }
    function h(function() external a) public returns (address) {
        return a.address;
    }
}
// ----
// f() -> 0x1234
// g() -> true, true
// h(function): left(0x000000000000123442424242) -> 0x1234
