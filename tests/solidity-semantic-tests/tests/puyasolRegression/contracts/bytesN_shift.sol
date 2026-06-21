// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer's BYTES mode
// (fuzz_gen.py --bytes). A bytesN bit shift `b << k` / `b >> k` HARD-ERRORED in the puya backend
// ("unsupported type cast from uint64 to bytes"): the generic integer-shift path coerced the bytesN
// operand through uint64, and uint64->bytes is unsupported (and uint64 can't hold bytes>8). Bitwise
// & | ^ ~ were already fine. Fix lowers the shift via biguint (asBiguint(b) shifted by k bits) then
// keeps the low N bytes (Solidity truncates to N), in SolFixedBytesBuilder::binary_op.
contract BytesNShift {
    function shl4(bytes4 a, uint8 k)    external pure returns (bytes4)  { return a << k; }
    function shr4(bytes4 a, uint8 k)    external pure returns (bytes4)  { return a >> k; }
    function shl32(bytes32 a, uint16 k) external pure returns (bytes32) { return a << k; }
    function shr1(bytes1 a, uint8 k)    external pure returns (bytes1)  { return a >> k; }
    function comp(bytes4 a, bytes4 b)   external pure returns (bytes4)  { return (a << 8) | b; }
}
