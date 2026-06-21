// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the GENERATIVE fuzzer (fuzz_gen.py).
// buildSignedExp (signed `**`) for a SUB-WORD type had two bugs:
//  (1) VALUE: an UNCHECKED result that overflows the type was not wrapped mod 2^bits, so the
//      negation `pow2N - absResult` underflowed the biguint subtraction and the AVM `b-` panicked
//      (e.g. int8 (-128)**3 = 2097152 > 256 → REVERT; EVM wraps to 0). Fixed by masking the
//      magnitude mod 2^bits for unchecked (checked keeps it raw so the overflow assert still fires).
//  (2) COMPOSITION: the result was a biguint, so `b ^ (a**3)` handed a biguint to a uint64 op
//      (puya compile error). Fixed by narrowing the sub-word exp result back to uint64.
contract SignedSubwordExp {
    function exp3u(int8 a)        external pure returns (int8)  { unchecked { return a ** 3; } }
    function exp3c(int8 a)        external pure returns (int8)  { return a ** 3; }              // checked
    function comp(int8 a, int8 b) external pure returns (int8)  { unchecked { return b ^ (a ** 3); } }
    function expU8(uint8 a)       external pure returns (uint8) { unchecked { return a ** 3; } }
    function expI16(int16 a)      external pure returns (int16) { unchecked { return a ** 2; } }
}
