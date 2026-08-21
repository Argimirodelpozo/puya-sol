// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM puya-sol regression: the
// encodePacked / bytesN padding-and-width family.
contract PackedPadding {
    // B12: solc legally widens bytesM→bytesN (right-padded); the compare and
    // bitwise lowerings previously padded only CONSTANT operands.
    function cmpMixed() public pure returns (bool, bool, bool) {
        bytes2 a = 0x6162;
        bytes4 b = 0x61620000;
        bytes4 c = 0x61620001;
        return (a == b, a < c, a >= c); // EVM: true, true, false
    }

    function bandMixed() public pure returns (bytes4) {
        bytes2 a = 0x6162;
        bytes4 b = 0xffffffff;
        return a & b; // EVM: 0x61620000
    }

    // B10: fixed arrays of 1-7-byte elements reverted at runtime
    // ("extract range beyond length").
    function packedFixedU32() public pure returns (bytes memory) {
        uint32[3] memory a = [uint32(1), 2, 3];
        return abi.encodePacked(a); // 96 bytes, LEFT-padded words
    }

    // B11: bytesN array elements were LEFT-padded; EVM pads RIGHT.
    function packedFixedBytes8() public pure returns (bytes memory) {
        bytes8[2] memory a = [bytes8(0x0102030405060708), bytes8(0x1112131415161718)];
        return abi.encodePacked(a); // 64 bytes, RIGHT-padded words
    }

    function packedDynBytes8() public pure returns (bytes memory) {
        bytes8[] memory a = new bytes8[](2);
        a[0] = 0x0102030405060708;
        a[1] = 0x1112131415161718;
        return abi.encodePacked(a); // 64 bytes, RIGHT-padded words
    }

    // C17: bool[] bodies are ARC-4 BIT-packed; the packed encoding emitted
    // the raw bit soup instead of one 0/1 word per element.
    function packedDynBool() public pure returns (bytes memory) {
        bool[] memory a = new bool[](3);
        a[0] = true;
        a[2] = true;
        return abi.encodePacked(a); // 96 bytes: ..01, ..00, ..01
    }

    function packedFixedBool() public pure returns (bytes memory) {
        bool[2] memory a = [true, false];
        return abi.encodePacked(a); // 64 bytes: ..01, ..00
    }

    // Regression guards for behaviour that was already right.
    function packedScalars() public pure returns (bytes memory) {
        // scalar packed values keep their natural width: 4 + 8 + 1 = 13 bytes
        return abi.encodePacked(uint32(0x01020304), bytes8(0x1112131415161718), true);
    }

    function packedSignedArray() public pure returns (bytes memory) {
        int64[2] memory a = [int64(-2), int64(3)];
        return abi.encodePacked(a); // 64 bytes: sign-extended ff..fe, 00..03
    }
}
