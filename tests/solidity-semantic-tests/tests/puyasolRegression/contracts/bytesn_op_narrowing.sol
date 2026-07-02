// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Regression: the result of a bytesN SHIFT or BITWISE binop (SolFixedBytesBuilder) carried a
// plain UNSIZED `bytes` wtype — the builder knew it was bytesN but the expression didn't. A
// subsequent bytesN(M->N) NARROWING conversion (convertToFixedBytes) reads the expression's
// BytesWType length; with length unknown it degenerated to a reinterpret no-op, so
// `uint32(bytes4(bytes32(x) << n))` btoi'd all 32 bytes -> "btoi arg too long" revert on EVERY
// input, and `bytes4(a & b)` kept all 32 bytes. Fixed by retagging both results with the sized
// bytes[N] wtype. The shift branch also had its own copy of the huge-amount truncation
// (implicitNumericCast low-64) -> now routed through shiftAmountToUint64. Found by the
// differential fuzzer (fixedbytes_probe truncShift).
contract C {
    function truncShift(uint256 x, uint8 n) external pure returns (uint32) {
        return uint32(bytes4(bytes32(x) << n));
    }
    function truncShiftHuge(uint256 x, uint256 n) external pure returns (uint32) {
        return uint32(bytes4(bytes32(x) << n));
    }
    function narrowAnd(uint256 x, uint256 y) external pure returns (uint32) {
        return uint32(bytes4(bytes32(x) & bytes32(y)));
    }
    function narrowOr(uint256 x, uint256 y) external pure returns (uint32) {
        return uint32(bytes4(bytes32(x) | bytes32(y)));
    }
    function narrowXor(uint256 x, uint256 y) external pure returns (uint32) {
        return uint32(bytes4(bytes32(x) ^ bytes32(y)));
    }
    // the docstring example from the bytesN-shift lowering: left-aligned semantics
    function shiftB6(uint48 v, uint8 n) external pure returns (uint48) {
        return uint48(bytes6(v) << n);
    }
}
