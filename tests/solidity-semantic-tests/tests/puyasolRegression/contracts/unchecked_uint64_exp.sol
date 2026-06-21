// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer's CAST mode
// (fuzz_gen.py --cast, via `(~uint64(uint256(d))) ** 3` = uint64_max ** 3). An UNCHECKED uint64 `a**k`
// whose power overflows 2^64 REVERTED: the AVM `exp` opcode is uint64-only and asserts on overflow,
// but Solidity wraps. The unchecked-exp wrapping path covered sub-word (m_bits<64) but uint64 (==64)
// fell in the gap (the same gap as unchecked uint64 subtraction). Fix routes uint64 unchecked Pow
// through biguint square-and-multiply + mod 2^64 + narrow. Add/Mult at uint64 already wrapped.
contract UncheckedUint64Exp {
    function exp2(uint64 a)  external pure returns (uint64) { unchecked { return a ** 2; } }
    function exp3(uint64 a)  external pure returns (uint64) { unchecked { return a ** 3; } }
    function exp2c(uint64 a) external pure returns (uint64) { return a ** 2; }                  // checked
    function comp(uint64 a)  external pure returns (uint64) { unchecked { return (a ** 2) | 7; } }  // compose
}
