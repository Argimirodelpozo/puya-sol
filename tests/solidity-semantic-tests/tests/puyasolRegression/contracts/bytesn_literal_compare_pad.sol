// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: comparing a bytesN value against a SHORTER string literal must
// right-pad the literal to N (EVM compares 32-byte left-aligned words).
// The literal arrived as a raw 2-byte StringConstant and was compared
// unpadded, so `x == "aa"` was false for every N > 2 — silent wrong result.
// Ordered compares had the same gap: unpadded "b" (0x62) sorted BELOW "aa"
// (0x6161) numerically where EVM's left-aligned words sort it above.
contract C {
    bytes22[2] data1;

    // the fuzzer's shape (conditional_expression_storage_memory_1, bytes2->bytes22)
    function viaTernary(bool cond) public returns (uint) {
        bytes22[2] memory x;
        x[0] = "aa";
        bytes22[2] memory y;
        y[0] = "bb";
        data1 = cond ? x : y;
        if (data1[0] == "aa") return 1;
        if (data1[0] == "bb") return 2;
        return 0;
    }

    function scalarEq() public pure returns (uint) {
        bytes22 v = "aa";
        if (v == "aa") return 1;
        return 0;
    }

    function widthSweep() public pure returns (uint r) {
        bytes8 a = "aa";
        bytes16 b = "aa";
        bytes32 c = "aa";
        if (a == "aa") r += 1;
        if (b == "aa") r += 2;
        if (c == "aa") r += 4;
        if (a != "ab") r += 8;
    }

    function ordered() public pure returns (uint r) {
        bytes3 b = "b";
        bytes3 aa = "aa";
        if (b > aa) r += 1;   // 0x62.. > 0x6161.. left-aligned
        if (b > "aa") r += 2; // literal rhs must pad before b>
        if (aa < "b") r += 4;
    }
}
