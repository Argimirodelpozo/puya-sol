// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Yul opcode handlers exercised directly (the generative fuzzer never emits assembly). fuzz_evm
// boundary-fuzzes the params and diffs AVM vs live EVM. Raw EVM semantics (unchecked, byte/word ops).
contract C {
    function sext(uint256 b, uint256 x) external pure returns (uint256 r) { assembly { r := signextend(b, x) } }
    function bytef(uint256 n, uint256 x) external pure returns (uint256 r) { assembly { r := byte(n, x) } }
    function amod(uint256 a, uint256 b, uint256 m) external pure returns (uint256 r) { assembly { r := addmod(a, b, m) } }
    function mmod(uint256 a, uint256 b, uint256 m) external pure returns (uint256 r) { assembly { r := mulmod(a, b, m) } }
    function shlf(uint256 s, uint256 x) external pure returns (uint256 r) { assembly { r := shl(s, x) } }
    function shrf(uint256 s, uint256 x) external pure returns (uint256 r) { assembly { r := shr(s, x) } }
    function sarf(uint256 s, uint256 x) external pure returns (uint256 r) { assembly { r := sar(s, x) } }
    function divf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := div(a, b) } }
    function sdivf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := sdiv(a, b) } }
    function modf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := mod(a, b) } }
    function smodf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := smod(a, b) } }
    function sltf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := slt(a, b) } }
    function sgtf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := sgt(a, b) } }
    function notf(uint256 a) external pure returns (uint256 r) { assembly { r := not(a) } }
    function mulf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := mul(a, b) } }
    function addf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := add(a, b) } }
    function subf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := sub(a, b) } }
}
