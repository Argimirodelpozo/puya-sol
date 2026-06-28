// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Regression: a SIGNED narrow-int (int8/16/32/64) RETURN from an external/inner contract call.
// The callee encodes a signed int return as a 32-byte uint256 (sign-extended), but the caller used
// to decode it with an 8-byte `btoi` → "btoi arg too long, got 32 bytes" → revert on EVERY such call
// (value-independent). Fixed by extracting the low 8 bytes (the canonical uint64-backed form) before
// btoi when the Solidity return type is signed. (Found by the cross-contract differential fuzzer.)
// Forwards widen+offset so the observable is a clean positive int; a broken decode reverts instead.
contract Cee {
    function r8(int256 a)  external pure returns (int8)  { return int8(a); }
    function r16(int256 a) external pure returns (int16) { return int16(a); }
    function r32(int256 a) external pure returns (int32) { return int32(a); }
    function r64(int256 a) external pure returns (int64) { return int64(a); }
    function u32(uint256 a) external pure returns (uint32) { return uint32(a); }   // unsigned control
}
contract Caller {
    Cee c;
    constructor() { c = new Cee(); }
    function g8(int256 a)  external returns (int256) { return int256(c.r8(a))  + 1000; }
    function g16(int256 a) external returns (int256) { return int256(c.r16(a)) + 1000; }
    function g32(int256 a) external returns (int256) { return int256(c.r32(a)) + 1000; }
    function g64(int256 a) external returns (int256) { return int256(c.r64(a)) + 1000; }
    function gu32(uint256 a) external returns (uint256) { return uint256(c.u32(a)) + 1000; }
}
