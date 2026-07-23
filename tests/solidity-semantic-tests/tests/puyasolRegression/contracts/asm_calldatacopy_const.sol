// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the calldatacopy no-op bug the fuzz_mem campaign found: a CONSTANT-offset
// calldatacopy in a function with no OTHER dynamic-calldata trigger (calldatasize
// / non-const calldataload / a dynamic param's .offset) never stood up the
// synthetic __cd_blob, so the copy was silently skipped and the memory read
// zero. Every value EVM-verified vs solc 0.8.20 (paris — no mcopy needed).
contract AsmCalldatacopyConst {
    // copy arg `a` (calldata offset 4) into memory slot 0, read back.
    function toLow(uint256 a, uint256 b) external pure returns (bytes32 r) {
        assembly { calldatacopy(0x200, 4, 32) r := mload(0x200) }
        a; b;
    }
    // copy arg `b` (offset 36) into memory slot 1 (0x1800, crosses 4096), read back.
    function toHigh(uint256 a, uint256 b) external pure returns (bytes32 r) {
        assembly { calldatacopy(0x1800, 36, 32) r := mload(0x1800) }
        a; b;
    }
    // partial copy: 8 bytes of arg `a` (its low 8 bytes at offset 4+24=28), padded.
    function partCopy(uint256 a) external pure returns (bytes32 r) {
        assembly { calldatacopy(0x100, 28, 8) r := mload(0x100) }
        a;
    }
}
