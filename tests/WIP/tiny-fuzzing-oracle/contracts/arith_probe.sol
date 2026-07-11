// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Second fixture for the LIVE-EVM ABI-driven fuzzer (fuzz_evm.py) — operations that were
// NEVER hand-modeled in oracle.py, to show the harness generalizes to arbitrary fixtures
// with zero new code (the real solc+EVM is the oracle). Standard ABI I/O where possible;
// sub-word / exotic widths included on purpose.
contract Arith {
    // bitwise
    function band(uint256 a, uint256 b) external pure returns (uint256) { return a & b; }
    function bor(uint256 a, uint256 b)  external pure returns (uint256) { return a | b; }
    function bxor(uint256 a, uint256 b) external pure returns (uint256) { return a ^ b; }
    function bnot(uint256 a)            external pure returns (uint256) { return ~a; }

    // comparisons (signed compare exercises the AVM's XOR-2^255 path)
    function ltU(uint256 a, uint256 b) external pure returns (uint256) { return a < b ? 1 : 0; }
    function ltI(int256 a, int256 b)   external pure returns (uint256) { return a < b ? 1 : 0; }
    function geI(int256 a, int256 b)   external pure returns (uint256) { return a >= b ? 1 : 0; }

    // min/max + checked negate/abs (negate reverts on INT256_MIN)
    function maxU(uint256 a, uint256 b) external pure returns (uint256) { return a > b ? a : b; }
    function negI(int256 a)             external pure returns (int256)  { return -a; }
    function absI(int256 a)             external pure returns (int256)  { return a < 0 ? -a : a; }

    // sub-word + exotic-width casts
    function castU8(uint256 x)  external pure returns (uint8)  { return uint8(x); }
    function castU96(uint256 x) external pure returns (uint96) { return uint96(x); }
    function widenI8(int8 x)    external pure returns (int256) { return x; }
    function int40RT(int256 x)  external pure returns (int256) { return int256(int40(x)); }

    // sub-word checked exponentiation + modulo (reverts on 0)
    function expU8(uint8 b, uint8 e)    external pure returns (uint8)  { return b ** e; }
    function modU(uint256 a, uint256 b) external pure returns (uint256) { return a % b; }
    function smodI(int256 a, int256 b)  external pure returns (int256)  { return a % b; }

    // bool param → the fuzzer should SKIP this (scalar-int filter)
    function notBool(bool x) external pure returns (bool) { return !x; }
}
