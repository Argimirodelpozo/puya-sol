// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM regression fixture (NOT vendored). Guards blob-backed ARRAY value-use.
//
// OZ EnumerableSet.values() pointer-pun, reduced: `result := store` type-puns a
// bytes32[] memory pointer into an address[]/uint256[] of identical layout.
//
// markAssemblyAggregates blob-backs EVERY real array touched by assembly, so
// `result` became a uint64 offset local. Copying the offset is correct in that
// model — but a bare VALUE use of a blob-backed array leaked the raw offset
// instead of materialising, so the subroutine returned uint64 while its
// declared return type was the array, and puya rejected the program:
//     invalid return type [PrimitiveIRType.uint64], expected EncodedType(...)
// That blocked gho (Aave GHO); the idiom appears in 6 of the replayed contracts.
//
// Fixed by materialising on value-use, mirroring the bytes/string case: EVM
// memory is [32-byte COUNT][elements] and ARC4 wants a 2-byte count prefix, so
// the re-encode swaps the header. Valid only while an element is 32 bytes in
// BOTH encodings — otherwise puya-sol now refuses loudly rather than emitting a
// mis-strided read. Verified against a live solc+EVM (80 calls, 0 divergences).
library Pun {
    function raw(uint256 n) internal pure returns (bytes32[] memory s) {
        s = new bytes32[](n);
        for (uint256 i = 0; i < n; i++) s[i] = bytes32(uint256(i + 1));
    }
    function asAddrs(uint256 n) internal pure returns (address[] memory result) {
        bytes32[] memory store = raw(n);
        assembly { result := store }
    }
    function asUints(uint256 n) internal pure returns (uint256[] memory result) {
        bytes32[] memory store = raw(n);
        assembly { result := store }
    }
}

contract EnumSetValues {
    function countAddrs(uint256 n) external pure returns (uint256) {
        require(n <= 12);
        return Pun.asAddrs(n).length;
    }
    function sumUints(uint256 n) external pure returns (uint256) {
        require(n <= 12);
        uint256[] memory v = Pun.asUints(n);
        uint256 t;
        for (uint256 i = 0; i < v.length; i++) t += v[i];
        return t;
    }
    function firstUint(uint256 n) external pure returns (uint256) {
        require(n >= 1 && n <= 12);
        return Pun.asUints(n)[0];
    }
    function lastUint(uint256 n) external pure returns (uint256) {
        require(n >= 1 && n <= 12);
        uint256[] memory v = Pun.asUints(n);
        return v[v.length - 1];
    }
}
