// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the STATEFUL differential fuzzer.
// A public struct auto-getter must sign-extend signed sub-word fields, same as an explicit
// field read. Single-field signed structs skipped projectStructFields entirely (read as a
// scalar): int16 → invalid AWST / compile error; int128 → returned +2^127 for INT128_MIN.
// Multi-field structs decoded each field unsigned. Now both flow through projectStructFields.
contract SignedStructGetter {
    struct One { int128 x; }                              // single-field, 64<N<256
    struct Small { int16 y; }                             // single-field, <=64 (was a compile error)
    struct Many { uint8 a; int16 b; uint64 c; int128 d; } // multi-field, mixed signedness
    One public one;
    Small public small;
    Many public many;
    function setOne(int128 v)            external { one.x = v; }
    function setSmall(int16 v)           external { small.y = v; }
    function setMany(int16 b, int128 d)  external { many.b = b; many.d = d; }
    // explicit reads — already sign-extended, used as oracles for the auto-getters
    function readOne()   external view returns (int128) { return one.x; }
    function readSmall() external view returns (int16)  { return small.y; }
}
