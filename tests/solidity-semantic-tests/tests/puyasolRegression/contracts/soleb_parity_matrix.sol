// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// sol-eb/ semantics matrix vs solc. Focus: cells the prior audits skipped —
// runtime exponentiation, bytesN shifts/bitwise/indexing, concat builtins —
// and their COMPOUND (`op=`) variants, which bypass SolBinaryOperation's
// routing and hit the raw eb builders (the June-audit signed-`-=` lesson).
contract SolEbParity {
    // ── runtime exponentiation ──
    function expBasics(uint256 b, uint256 e) public pure returns (uint256) {
        return b ** e;
    }

    function expZeroZero() public pure returns (uint256) {
        uint256 z = 0;
        return z ** z; // EVM: 1
    }

    function expSignedNegBase(int256 b, uint256 e) public pure returns (int256) {
        return b ** e; // (-2)**3 == -8; (-2)**2 == 4
    }

    function expOverflowPanics() public pure returns (uint256) {
        uint256 b = 2;
        uint256 e = 256;
        return b ** e; // checked: Panic(0x11)
    }

    function expUncheckedWraps() public pure returns (uint256) {
        uint256 b = 2;
        uint256 e = 256;
        unchecked { return b ** e; } // wraps to 0
    }

    function expNarrow(uint8 b, uint8 e) public pure returns (uint8) {
        return b ** e; // 3**4=81 fits; width-checked
    }

    function expNarrowOverflow() public pure returns (uint8) {
        uint8 b = 3;
        uint8 e = 5;
        return b ** e; // 243? no: 3**5=243 fits uint8. Use 4**4=256 -> panic
    }

    function expNarrowPanics() public pure returns (uint8) {
        uint8 b = 4;
        uint8 e = 4;
        return b ** e; // 256 -> Panic(0x11)
    }

    // ── bytesN shifts / bitwise ──
    function bytesShiftLeft() public pure returns (bytes4) {
        bytes4 b = 0x11223344;
        return b << 8; // 0x22334400
    }

    function bytesShiftLeftOdd() public pure returns (bytes4) {
        bytes4 b = 0x11223344;
        return b << 4; // 0x12233440
    }

    function bytesShiftRight() public pure returns (bytes4) {
        bytes4 b = 0x11223344;
        return b >> 12; // 0x00011223
    }

    function bytesShiftCompound() public pure returns (bytes4) {
        bytes4 b = 0x11223344;
        b <<= 16; // 0x33440000
        return b;
    }

    function bytesShiftOverWidth() public pure returns (bytes4) {
        bytes4 b = 0x11223344;
        return b >> 40; // 0x00000000 (shift >= 32 bits)
    }

    function bytesBitwise() public pure returns (bytes4, bytes4, bytes4) {
        bytes4 a = 0xF0F0F0F0;
        bytes4 b = 0x0FF00FF0;
        return (a & b, a | b, a ^ b); // 00F000F0, FFF0FFF0, FF00FF00
    }

    function bytesNot() public pure returns (bytes4) {
        bytes4 a = 0xF0F0F0F0;
        return ~a; // 0x0F0F0F0F
    }

    function bytesIndex(uint256 i) public pure returns (bytes1) {
        bytes4 b = 0x11223344;
        return b[i]; // b[2] == 0x33; b[7] -> Panic(0x32)
    }

    // ── concat builtins ──
    function bytesConcatMixed() public pure returns (bytes memory) {
        bytes2 a = 0xAABB;
        bytes memory m = hex"CCDD";
        // bytesN contributes exactly N bytes (no padding)
        return bytes.concat(a, m, hex"EE");
    }

    function stringConcat3() public pure returns (string memory) {
        return string.concat("ab", "", "cd");
    }
}
