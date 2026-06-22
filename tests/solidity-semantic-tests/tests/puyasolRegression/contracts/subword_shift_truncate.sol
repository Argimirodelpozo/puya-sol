// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (--cast).
// Solidity truncates `x << n` to the operand type's width (shifts never overflow-check, in checked OR
// unchecked code): `uint8(254) << 1` is 252, not 508. The AVM lowering ran the shift in biguint and only
// wrapped to 2^256 (buildBigUIntShift) — it never masked back to 2^bits for sub-word/uint64 types. The
// return path re-masks, so a bare `return a << 1` hid the bug; it only surfaced when the shift result was
// consumed mid-expression (e.g. a comparison), where the un-truncated 508 flipped `255 >= (a<<1)`.
// FIX: mask unsigned sub-word/uint64 LShift back to 2^bits after buildBigUIntShift.
contract SubwordShiftTruncate {
    // comparison consumes the shift before any re-mask — the bug's exposer
    function shlCmpU8(uint8 a) external pure returns (bool) { unchecked { return 255 >= (a << 1); } }
    function comboChkU8(uint8 a) external pure returns (bool) { return 255 > ((~a) << 1); } // checked
    function shlCmpU16(uint16 a) external pure returns (bool) { unchecked { return 65535 >= (a << 1); } }
    function shlCmpU64(uint64 a) external pure returns (bool) { unchecked { return type(uint64).max >= (a << 1); } }
    // value still correct when consumed in further arithmetic (low bits only)
    function shlMaskU8(uint8 a) external pure returns (uint8) { unchecked { return (a << 1) | 1; } }
}
