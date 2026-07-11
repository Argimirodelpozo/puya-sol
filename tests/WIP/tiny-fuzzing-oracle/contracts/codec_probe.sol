// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Spike fixture for differential fuzzing. Every function takes/returns STANDARD
// ABI types (int256/uint256) so algosdk decodes cleanly — the divergence-prone
// codec/arithmetic logic (sub-word sign-extension, checked overflow, signed div)
// is exercised INTERNALLY. The oracle (oracle.py) models the EVM result; any diff
// on this pure/view computational subset is a real divergence (no by-design noise).
contract Probe {
    // sub-word signed truncate+widen (the int24/intN sign-extension class)
    function int24RT(int256 x) external pure returns (int256) { return int256(int24(x)); }
    function int8RT(int256 x)  external pure returns (int256) { return int256(int8(x)); }
    function int128RT(int256 x) external pure returns (int256) { return int256(int128(x)); }
    function uint24RT(uint256 x) external pure returns (uint256) { return uint256(uint24(x)); }

    // checked 256-bit arithmetic (reverts on overflow / underflow)
    function addU256(uint256 a, uint256 b) external pure returns (uint256) { return a + b; }
    function subU256(uint256 a, uint256 b) external pure returns (uint256) { return a - b; }
    function mulU256(uint256 a, uint256 b) external pure returns (uint256) { return a * b; }

    // checked sub-word arithmetic (uint8 add reverts > 255)
    function addU8(uint256 a, uint256 b) external pure returns (uint256) {
        return uint256(uint8(a) + uint8(b));
    }

    // signed division (truncate toward zero; reverts on /0 and INT256_MIN / -1)
    function divI256(int256 a, int256 b) external pure returns (int256) { return a / b; }
    function modI256(int256 a, int256 b) external pure returns (int256) { return a % b; }

    // abi round-trip through a sub-word signed type
    function abiRTInt128(int256 x) external pure returns (int256) {
        return int256(abi.decode(abi.encode(int128(x)), (int128)));
    }

    // shifts — the ≥256 saturation edge (shl/shr→0, sar→0/-1)
    function shlU256(uint256 x, uint256 s) external pure returns (uint256) { return x << s; }
    function shrU256(uint256 x, uint256 s) external pure returns (uint256) { return x >> s; }
    function sarI256(int256 x, uint256 s) external pure returns (int256) { return x >> s; }

    // addmod/mulmod — m==0 returns 0 (NOT a revert) and the 512-bit intermediate
    function addmodU(uint256 a, uint256 b, uint256 m) external pure returns (uint256) { return addmod(a, b, m); }
    function mulmodU(uint256 a, uint256 b, uint256 m) external pure returns (uint256) { return mulmod(a, b, m); }

    // checked exponentiation (reverts on overflow; 0**0==1)
    function expU(uint256 b, uint256 e) external pure returns (uint256) { return b ** e; }

    // unchecked wrapping add (no revert; wraps mod 2^256)
    function uncheckedAdd(uint256 a, uint256 b) external pure returns (uint256) {
        unchecked { return a + b; }
    }
}
