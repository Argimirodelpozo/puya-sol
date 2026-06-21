// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the GENERATIVE fuzzer's CONTROL-FLOW mode
// (fuzz_gen.py --cf). An UNCHECKED uint64 `a - b` with a < b reverted: the raw uint64 `-` opcode
// panics on underflow, but Solidity wraps to a + 2^64 - b. The wrapping-subtraction fix covered
// m_bits < 64 (sub-word: a + 2^N fits uint64) and m_bits > 64 (biguint); uint64 (== 64) fell in the
// gap (a + 2^64 overflows uint64). Fix routes uint64 unchecked Sub through the biguint wrapping
// subtract, then narrows the result back to uint64.
contract UncheckedUint64Sub {
    function sub(uint64 a, uint64 b)            external pure returns (uint64) { unchecked { return a - b; } }
    function subc(uint64 a, uint64 b)           external pure returns (uint64) { return a - b; }            // checked
    function comp(uint64 a, uint64 b, uint64 c) external pure returns (uint64) { unchecked { return (a - b) | c; } }
}
