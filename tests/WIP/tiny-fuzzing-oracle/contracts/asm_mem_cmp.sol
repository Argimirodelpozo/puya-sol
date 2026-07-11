// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Yul memory + comparison/logic + keccak handlers (the generative fuzzer never emits assembly).
// fuzz_evm boundary-fuzzes params and diffs AVM vs live EVM. Offsets masked to slot 0 to stay in-bounds.
contract C {
    // NOTE: param-as-memory-offset is a recorded bug (asm-param-as-memory-offset), excluded here.
    // Const / let-local offsets are correct, so probe those.
    function msConst(uint256 v) external pure returns (uint256 r) {
        assembly { mstore(96, v) r := mload(96) }
    }
    function ms8Const(uint256 v) external pure returns (uint256 r) {
        assembly { mstore8(96, v) r := mload(96) }
    }
    function msLet(uint256 o, uint256 v) external pure returns (uint256 r) {
        assembly { let off := mod(o, 256) mstore(off, v) r := mload(off) }
    }
    function msOverlap(uint256 v) external pure returns (uint256 r) {
        assembly { mstore(0, v) r := mload(1) }  // read shifted by one byte; byte 32 is 0
    }
    function ltf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := lt(a, b) } }
    function gtf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := gt(a, b) } }
    function eqf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := eq(a, b) } }
    function iszerof(uint256 a) external pure returns (uint256 r) { assembly { r := iszero(a) } }
    function andf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := and(a, b) } }
    function orf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := or(a, b) } }
    function xorf(uint256 a, uint256 b) external pure returns (uint256 r) { assembly { r := xor(a, b) } }
    function kec(uint256 v) external pure returns (bytes32 r) { assembly { mstore(0, v) r := keccak256(0, 32) } }
    function kec0(uint256 v) external pure returns (bytes32 r) { assembly { mstore(0, v) r := keccak256(0, 0) } }
}
