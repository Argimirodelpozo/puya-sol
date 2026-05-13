// ADAPTED-FOR-ALGORAND-ARC4
// Usual EVM behavior: method selectors are the first 4 bytes of keccak256 of
// the signature, so `fow()` has selector 0xd88e0b00. Any call with fewer than
// 4 bytes of calldata (or bytes that don't match any selector) routes to the
// fallback. The original test used `d88e0b00` for the full-selector case and
// shorter prefixes (`d88e0b`, `d88e`, `d8`) to exercise fallback dispatch.
//
// On Algorand/ARC4 method selectors are the first 4 bytes of sha512/256 of
// the ARC4 signature instead, so `fow()` has selector 0x12b87db6. This file
// substitutes the ARC4 selector everywhere the original used the EVM one, and
// keeps the short-prefix inputs as prefixes of the ARC4 selector so the
// "short data routes to fallback" semantics being tested are preserved.
contract A {
    uint public x;
    // EVM signature: 0xd88e0b00 ; ARC4 signature: 0x12b87db6
    function fow() public { x = 3; }
    fallback () external { x = 2; }
}
// ----
// (): hex"12b87d"
// x() -> 2
// (): hex"12b87db6"
// x() -> 3
// (): hex"12b8"
// x() -> 2
// (): hex"12b87db6"
// x() -> 3
// (): hex"12"
// x() -> 2
