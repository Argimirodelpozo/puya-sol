// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Regression: a runtime shift AMOUNT >= 2^64 (biguint-typed uint256) was low-64-truncated by
// the biguint->uint64 coercion BEFORE the >=256 saturation guards ran: `x >> 2^128` shifted by
// (2^128 mod 2^64) = 0 and returned x unchanged, `x >> (2^128+5)` shifted by 5 — where the EVM
// saturates for ANY amount >= 256 (shl/shr -> 0, sar -> 0 or -1, EIP-145). Fixed by
// eb::shiftAmountToUint64: clamp at the biguint level (amount >= 256 -> 256, which the shift
// builders saturate on; amount < 256 -> the low-64 extract is exact). Found by the differential
// fuzzer (codec_probe sarI256 / arith_edge sarI8).
contract C {
    function sar(int256 x, uint256 s) external pure returns (int256) { return x >> s; }
    function sar8(int8 x, uint256 s) external pure returns (int8) { return x >> s; }
    function shr(uint256 x, uint256 s) external pure returns (uint256) { return x >> s; }
    function shl(uint256 x, uint256 s) external pure returns (uint256) { return x << s; }
}
