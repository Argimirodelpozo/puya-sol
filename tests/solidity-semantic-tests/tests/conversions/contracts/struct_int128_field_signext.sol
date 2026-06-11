// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// CUSTOM puya-sol test contract (NOT vendored from the upstream Solidity
// semantic suite) — guards a compiler fix. A signed sub-256 struct FIELD (e.g.
// int128) is decoded as its raw N-bit two's complement; it must be sign-extended
// to canonical 256-bit on read so `s.x == scalar` / arithmetic match. Before the
// fix the sub-64-bit case was handled but the 64<N<256 (biguint-backed) case was
// not — eq(-5) returned false (field 2^128-5 vs scalar 2^256-5). Same bug class
// as the int128[] array-element and transient read fixes.
contract C {
    struct S { int128 x; uint64 tag; }
    struct U { uint128 x; }
    function eq(int128 v) external pure returns (bool) { S memory s = S(v, 9); return s.x == v; }
    function widen(int128 v) external pure returns (int256) { S memory s = S(v, 9); return int256(s.x); }
    function arith(int128 v) external pure returns (int256) { S memory s = S(v, 9); return int256(s.x) + 1; }
    function unsignedOk(uint128 v) external pure returns (bool) { U memory u = U(v); return u.x == v; }
}
